/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/queue.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <assert.h>
#include <stdbool.h>

#include <eal_trace_internal.h>
#include <rte_common.h>
#include <rte_interrupts.h>
#include <rte_thread.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_debug.h>
#include <rte_log.h>
#include <rte_errno.h>
#include <rte_spinlock.h>
#include <rte_pause.h>
#include <rte_vfio.h>

#include "eal_private.h"

#include <x86gprintrin.h>

// #define __USE_GNU
#include <syscall.h>
#include <pthread.h>
#include <sched.h>

#ifndef __NR_uintr_register_handler
#define __NR_uintr_register_handler	471
#define __NR_uintr_unregister_handler	472
#define __NR_uintr_create_fd		473
#define __NR_uintr_register_sender	474
#define __NR_uintr_unregister_sender	475
#define __NR_uintr_wait			476
#endif

#define uintr_register_handler(handler, flags)	syscall(__NR_uintr_register_handler, handler, flags)
#define uintr_unregister_handler(flags)		syscall(__NR_uintr_unregister_handler, flags)
#define uintr_create_fd(vector, flags)		syscall(__NR_uintr_create_fd, vector, flags)
#define uintr_register_sender(fd, flags)	syscall(__NR_uintr_register_sender, fd, flags)
#define uintr_unregister_sender(ipi_idx, flags)	syscall(__NR_uintr_unregister_sender, ipi_idx, flags)
#define uintr_wait(flags)			syscall(__NR_uintr_wait, flags)
uint64_t temp = 1;

void __attribute__((interrupt))__attribute__((target("general-regs-only", "inline-all-stringops")))
uintr_handler(struct __uintr_frame *ui_frame,
	      unsigned long long vector)
{
	temp ++;
}

#define EAL_INTR_EPOLL_WAIT_FOREVER (-1)
#define NB_OTHER_INTR               1



static RTE_DEFINE_PER_LCORE(int, _epfd) = -1; /**< epoll fd per thread */

/**
 * union for pipe fds.
 */
union intr_pipefds{
	struct {
		int pipefd[2];
	};
	struct {
		int readfd;
		int writefd;
	};
};

/**
 * union buffer for reading on different devices
 */
union rte_intr_read_buffer {
	int uio_intr_count;              /* for uio device */
#ifdef VFIO_PRESENT
	uint64_t vfio_intr_count;        /* for vfio device */
#endif
	uint64_t timerfd_num;            /* for timerfd */
	char charbuf[16];                /* for others */
};

TAILQ_HEAD(rte_intr_cb_list, rte_intr_callback);
TAILQ_HEAD(rte_intr_source_list, rte_intr_source);

struct rte_intr_callback {
	TAILQ_ENTRY(rte_intr_callback) next;
	rte_intr_callback_fn cb_fn;  /**< callback address */
	void *cb_arg;                /**< parameter for callback */
	uint8_t pending_delete;      /**< delete after callback is called */
	rte_intr_unregister_callback_fn ucb_fn; /**< fn to call before cb is deleted */
};

struct rte_intr_source {
	TAILQ_ENTRY(rte_intr_source) next;
	struct rte_intr_handle *intr_handle; /**< interrupt handle */
	struct rte_intr_cb_list callbacks;  /**< user callbacks */
	uint32_t active;
};

/* global spinlock for interrupt data operation */
static rte_spinlock_t intr_lock = RTE_SPINLOCK_INITIALIZER;

/* union buffer for pipe read/write */
static union intr_pipefds intr_pipe;

/* interrupt sources list */
static struct rte_intr_source_list intr_sources;

/* interrupt handling thread */
static rte_thread_t intr_thread;

/* VFIO interrupts */
#ifdef VFIO_PRESENT

#define IRQ_SET_BUF_LEN  (sizeof(struct vfio_irq_set) + sizeof(int))
/* irq set buffer length for queue interrupts and LSC interrupt */
#define MSIX_IRQ_SET_BUF_LEN (sizeof(struct vfio_irq_set) + \
			      sizeof(int) * (RTE_MAX_RXTX_INTR_VEC_ID + 1))

/* enable legacy (INTx) interrupts */
static int
vfio_enable_intx(const struct rte_intr_handle *intr_handle) {
	struct vfio_irq_set *irq_set;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	int len, ret, vfio_dev_fd;
	int *fd_ptr;

	len = sizeof(irq_set_buf);

	/* enable INTx */
	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 1;
	irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
	irq_set->start = 0;
	fd_ptr = (int *) &irq_set->data;
	*fd_ptr = rte_intr_fd_get(intr_handle);

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error enabling INTx interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}

	/* unmask INTx after enabling */
	memset(irq_set, 0, len);
	len = sizeof(struct vfio_irq_set);
	irq_set->argsz = len;
	irq_set->count = 1;
	irq_set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK;
	irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
	irq_set->start = 0;

	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error unmasking INTx interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}
	return 0;
}

/* disable legacy (INTx) interrupts */
static int
vfio_disable_intx(const struct rte_intr_handle *intr_handle) {
	struct vfio_irq_set *irq_set;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	int len, ret, vfio_dev_fd;

	len = sizeof(struct vfio_irq_set);

	/* mask interrupts before disabling */
	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 1;
	irq_set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_MASK;
	irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
	irq_set->start = 0;

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error masking INTx interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}

	/* disable INTx*/
	memset(irq_set, 0, len);
	irq_set->argsz = len;
	irq_set->count = 0;
	irq_set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_INTX_IRQ_INDEX;
	irq_set->start = 0;

	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error disabling INTx interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}
	return 0;
}

/* unmask/ack legacy (INTx) interrupts */
static int
vfio_ack_intx(const struct rte_intr_handle *intr_handle)
{
	struct vfio_irq_set irq_set;
	int vfio_dev_fd;

	/* unmask INTx */
	memset(&irq_set, 0, sizeof(irq_set));
	irq_set.argsz = sizeof(irq_set);
	irq_set.count = 1;
	irq_set.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_UNMASK;
	irq_set.index = VFIO_PCI_INTX_IRQ_INDEX;
	irq_set.start = 0;

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	if (ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, &irq_set)) {
		EAL_LOG(ERR, "Error unmasking INTx interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}
	return 0;
}

/* enable MSI interrupts */
static int
vfio_enable_msi(const struct rte_intr_handle *intr_handle) {
	int len, ret;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	struct vfio_irq_set *irq_set;
	int *fd_ptr, vfio_dev_fd;

	len = sizeof(irq_set_buf);

	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 1;
	irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_MSI_IRQ_INDEX;
	irq_set->start = 0;
	fd_ptr = (int *) &irq_set->data;
	*fd_ptr = rte_intr_fd_get(intr_handle);

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error enabling MSI interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}
	return 0;
}

/* disable MSI interrupts */
static int
vfio_disable_msi(const struct rte_intr_handle *intr_handle) {
	struct vfio_irq_set *irq_set;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	int len, ret, vfio_dev_fd;

	len = sizeof(struct vfio_irq_set);

	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 0;
	irq_set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_MSI_IRQ_INDEX;
	irq_set->start = 0;

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);
	if (ret)
		EAL_LOG(ERR, "Error disabling MSI interrupts for fd %d",
			rte_intr_fd_get(intr_handle));

	return ret;
}

/* enable MSI-X interrupts */
static int
vfio_enable_msix(const struct rte_intr_handle *intr_handle) {
	int len, ret;
	char irq_set_buf[MSIX_IRQ_SET_BUF_LEN];
	struct vfio_irq_set *irq_set;
	int *fd_ptr, vfio_dev_fd, i;

	len = sizeof(irq_set_buf);

	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	/* 0 < irq_set->count < RTE_MAX_RXTX_INTR_VEC_ID + 1 */
	irq_set->count = rte_intr_max_intr_get(intr_handle) ?
		(rte_intr_max_intr_get(intr_handle) >
		 RTE_MAX_RXTX_INTR_VEC_ID + 1 ?	RTE_MAX_RXTX_INTR_VEC_ID + 1 :
		 rte_intr_max_intr_get(intr_handle)) : 1;

	irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_MSIX_IRQ_INDEX;
	irq_set->start = 0;
	fd_ptr = (int *) &irq_set->data;
	/* INTR vector offset 0 reserve for non-efds mapping */
	fd_ptr[RTE_INTR_VEC_ZERO_OFFSET] = rte_intr_fd_get(intr_handle);
	for (i = 0; i < rte_intr_nb_efd_get(intr_handle); i++) {
		fd_ptr[RTE_INTR_VEC_RXTX_OFFSET + i] =
			rte_intr_efds_index_get(intr_handle, i);
	}

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error enabling MSI-X interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}

	return 0;
}

/* enable MSI-X interrupts */
static int
vfio_enable_msix_uintr(const struct rte_intr_handle *intr_handle) {
	int len, ret;
	char irq_set_buf[MSIX_IRQ_SET_BUF_LEN];
	struct vfio_irq_set *irq_set;
	int *fd_ptr, vfio_dev_fd, i;

	len = sizeof(irq_set_buf);

	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	/* 0 < irq_set->count < RTE_MAX_RXTX_INTR_VEC_ID + 1 */
	irq_set->count = 1;


	irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_MSIX_IRQ_INDEX;
	irq_set->start = 0;
	fd_ptr = (int *) &irq_set->data;
	/* INTR vector offset 0 reserve for non-efds mapping */
	fd_ptr[RTE_INTR_VEC_ZERO_OFFSET] = rte_intr_fd_get(intr_handle);
	for (i = 0; i < rte_intr_nb_efd_get(intr_handle); i++) {
		fd_ptr[RTE_INTR_VEC_RXTX_OFFSET + i] =
			rte_intr_efds_index_get(intr_handle, i);
		EAL_LOG(ERR, "Enable MSI-X interrupts for fd %d offset: %d",
			fd_ptr[RTE_INTR_VEC_RXTX_OFFSET + i], RTE_INTR_VEC_RXTX_OFFSET + i);
	}

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error enabling MSI-X interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}


	irq_set->count = rte_intr_max_intr_get(intr_handle) ?
		(rte_intr_max_intr_get(intr_handle) >
		 RTE_MAX_RXTX_INTR_VEC_ID + 1 ?	RTE_MAX_RXTX_INTR_VEC_ID + 1 :
		 rte_intr_max_intr_get(intr_handle)) : 1;
	irq_set->count --;
	if(irq_set->count == 0)
		return 0;


	#define VFIO_IRQ_SET_DATA_UINTRFD	(1 << 6) /* Data is uintrfd (s32) */ // TOFO: 这个还需要存在吗？
	irq_set->flags = VFIO_IRQ_SET_DATA_UINTRFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	EAL_LOG(ERR, "Enable MSI-X interrupts for flag %u with uintrfd",
		irq_set->flags);
	irq_set->index = VFIO_PCI_MSIX_IRQ_INDEX;
	irq_set->start = 1;
	fd_ptr = (int *) &irq_set->data;
	/* INTR vector offset 0 reserve for non-efds mapping */
	for (i = 0; i < rte_intr_nb_efd_get(intr_handle); i++) {
		fd_ptr[RTE_INTR_VEC_ZERO_OFFSET + i] =
			rte_intr_efds_index_get(intr_handle, i);
		EAL_LOG(ERR, "Enable MSI-X interrupts for fd %d offset: %d",
			fd_ptr[RTE_INTR_VEC_ZERO_OFFSET + i], RTE_INTR_VEC_ZERO_OFFSET + i);
	}

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error enabling MSI-X interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}

	return 0;
}

/* disable MSI-X interrupts */
static int
vfio_disable_msix(const struct rte_intr_handle *intr_handle) {
	struct vfio_irq_set *irq_set;
	char irq_set_buf[MSIX_IRQ_SET_BUF_LEN];
	int len, ret, vfio_dev_fd;

	len = sizeof(struct vfio_irq_set);

	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 0;
	irq_set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_MSIX_IRQ_INDEX;
	irq_set->start = 0;

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret)
		EAL_LOG(ERR, "Error disabling MSI-X interrupts for fd %d",
			rte_intr_fd_get(intr_handle));

	return ret;
}

#ifdef HAVE_VFIO_DEV_REQ_INTERFACE
/* enable req notifier */
static int
vfio_enable_req(const struct rte_intr_handle *intr_handle)
{
	int len, ret;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	struct vfio_irq_set *irq_set;
	int *fd_ptr, vfio_dev_fd;

	len = sizeof(irq_set_buf);

	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 1;
	irq_set->flags = VFIO_IRQ_SET_DATA_EVENTFD |
			 VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_REQ_IRQ_INDEX;
	irq_set->start = 0;
	fd_ptr = (int *) &irq_set->data;
	*fd_ptr = rte_intr_fd_get(intr_handle);

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		EAL_LOG(ERR, "Error enabling req interrupts for fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}

	return 0;
}

/* disable req notifier */
static int
vfio_disable_req(const struct rte_intr_handle *intr_handle)
{
	struct vfio_irq_set *irq_set;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	int len, ret, vfio_dev_fd;

	len = sizeof(struct vfio_irq_set);

	irq_set = (struct vfio_irq_set *) irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 0;
	irq_set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = VFIO_PCI_REQ_IRQ_INDEX;
	irq_set->start = 0;

	vfio_dev_fd = rte_intr_dev_fd_get(intr_handle);
	ret = ioctl(vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret)
		EAL_LOG(ERR, "Error disabling req interrupts for fd %d",
			rte_intr_fd_get(intr_handle));

	return ret;
}
#endif
#endif

static int
uio_intx_intr_disable(const struct rte_intr_handle *intr_handle)
{
	unsigned char command_high;
	int uio_cfg_fd;

	/* use UIO config file descriptor for uio_pci_generic */
	uio_cfg_fd = rte_intr_dev_fd_get(intr_handle);
	if (uio_cfg_fd < 0 || pread(uio_cfg_fd, &command_high, 1, 5) != 1) {
		EAL_LOG(ERR,
			"Error reading interrupts status for fd %d",
			uio_cfg_fd);
		return -1;
	}
	/* disable interrupts */
	command_high |= 0x4;
	if (pwrite(uio_cfg_fd, &command_high, 1, 5) != 1) {
		EAL_LOG(ERR,
			"Error disabling interrupts for fd %d",
			uio_cfg_fd);
		return -1;
	}

	return 0;
}

static int
uio_intx_intr_enable(const struct rte_intr_handle *intr_handle)
{
	unsigned char command_high;
	int uio_cfg_fd;

	/* use UIO config file descriptor for uio_pci_generic */
	uio_cfg_fd = rte_intr_dev_fd_get(intr_handle);
	if (uio_cfg_fd < 0 || pread(uio_cfg_fd, &command_high, 1, 5) != 1) {
		EAL_LOG(ERR,
			"Error reading interrupts status for fd %d",
			uio_cfg_fd);
		return -1;
	}
	/* enable interrupts */
	command_high &= ~0x4;
	if (pwrite(uio_cfg_fd, &command_high, 1, 5) != 1) {
		EAL_LOG(ERR,
			"Error enabling interrupts for fd %d",
			uio_cfg_fd);
		return -1;
	}

	return 0;
}

static int
uio_intr_disable(const struct rte_intr_handle *intr_handle)
{
	const int value = 0;

	if (rte_intr_fd_get(intr_handle) < 0 ||
	    write(rte_intr_fd_get(intr_handle), &value, sizeof(value)) < 0) {
		EAL_LOG(ERR, "Error disabling interrupts for fd %d (%s)",
			rte_intr_fd_get(intr_handle), strerror(errno));
		return -1;
	}
	return 0;
}

static int
uio_intr_enable(const struct rte_intr_handle *intr_handle)
{
	const int value = 1;

	if (rte_intr_fd_get(intr_handle) < 0 ||
	    write(rte_intr_fd_get(intr_handle), &value, sizeof(value)) < 0) {
		EAL_LOG(ERR, "Error enabling interrupts for fd %d (%s)",
			rte_intr_fd_get(intr_handle), strerror(errno));
		return -1;
	}
	return 0;
}

int
rte_intr_callback_register(const struct rte_intr_handle *intr_handle,
			rte_intr_callback_fn cb, void *cb_arg)
{
	int ret, wake_thread;
	struct rte_intr_source *src;
	struct rte_intr_callback *callback;

	wake_thread = 0;

	/* first do parameter checking */
	if (rte_intr_fd_get(intr_handle) < 0 || cb == NULL) {
		EAL_LOG(ERR, "Registering with invalid input parameter");
		return -EINVAL;
	}

	/* allocate a new interrupt callback entity */
	callback = calloc(1, sizeof(*callback));
	if (callback == NULL) {
		EAL_LOG(ERR, "Can not allocate memory");
		return -ENOMEM;
	}
	callback->cb_fn = cb;
	callback->cb_arg = cb_arg;
	callback->pending_delete = 0;
	callback->ucb_fn = NULL;

	rte_spinlock_lock(&intr_lock);

	/* check if there is at least one callback registered for the fd */
	TAILQ_FOREACH(src, &intr_sources, next) {
		if (rte_intr_fd_get(src->intr_handle) == rte_intr_fd_get(intr_handle)) {
			/* we had no interrupts for this */
			if (TAILQ_EMPTY(&src->callbacks))
				wake_thread = 1;

			TAILQ_INSERT_TAIL(&(src->callbacks), callback, next);
			ret = 0;
			break;
		}
	}

	/* no existing callbacks for this - add new source */
	if (src == NULL) {
		src = calloc(1, sizeof(*src));
		if (src == NULL) {
			EAL_LOG(ERR, "Can not allocate memory");
			ret = -ENOMEM;
			free(callback);
			callback = NULL;
		} else {
			src->intr_handle = rte_intr_instance_dup(intr_handle);
			if (src->intr_handle == NULL) {
				EAL_LOG(ERR, "Can not create intr instance");
				ret = -ENOMEM;
				free(callback);
				callback = NULL;
				free(src);
				src = NULL;
			} else {
				TAILQ_INIT(&src->callbacks);
				TAILQ_INSERT_TAIL(&(src->callbacks), callback,
						  next);
				TAILQ_INSERT_TAIL(&intr_sources, src, next);
				wake_thread = 1;
				ret = 0;
			}
		}
	}

	rte_spinlock_unlock(&intr_lock);

	/**
	 * check if need to notify the pipe fd waited by epoll_wait to
	 * rebuild the wait list.
	 */
	if (wake_thread)
		if (write(intr_pipe.writefd, "1", 1) < 0)
			ret = -EPIPE;

	rte_eal_trace_intr_callback_register(intr_handle, cb, cb_arg, ret);
	return ret;
}

int
rte_intr_callback_unregister_pending(const struct rte_intr_handle *intr_handle,
				rte_intr_callback_fn cb_fn, void *cb_arg,
				rte_intr_unregister_callback_fn ucb_fn)
{
	int ret;
	struct rte_intr_source *src;
	struct rte_intr_callback *cb, *next;

	/* do parameter checking first */
	if (rte_intr_fd_get(intr_handle) < 0) {
		EAL_LOG(ERR, "Unregistering with invalid input parameter");
		return -EINVAL;
	}

	rte_spinlock_lock(&intr_lock);

	/* check if the interrupt source for the fd is existent */
	TAILQ_FOREACH(src, &intr_sources, next) {
		if (rte_intr_fd_get(src->intr_handle) == rte_intr_fd_get(intr_handle))
			break;
	}

	/* No interrupt source registered for the fd */
	if (src == NULL) {
		ret = -ENOENT;

	/* only usable if the source is active */
	} else if (src->active == 0) {
		ret = -EAGAIN;

	} else {
		ret = 0;

		/* walk through the callbacks and mark all that match. */
		for (cb = TAILQ_FIRST(&src->callbacks); cb != NULL; cb = next) {
			next = TAILQ_NEXT(cb, next);
			if (cb->cb_fn == cb_fn && (cb_arg == (void *)-1 ||
					cb->cb_arg == cb_arg)) {
				cb->pending_delete = 1;
				cb->ucb_fn = ucb_fn;
				ret++;
			}
		}
	}

	rte_spinlock_unlock(&intr_lock);

	return ret;
}

int
rte_intr_callback_unregister(const struct rte_intr_handle *intr_handle,
			rte_intr_callback_fn cb_fn, void *cb_arg)
{
	int ret;
	struct rte_intr_source *src;
	struct rte_intr_callback *cb, *next;

	/* do parameter checking first */
	if (rte_intr_fd_get(intr_handle) < 0) {
		EAL_LOG(ERR, "Unregistering with invalid input parameter");
		return -EINVAL;
	}

	rte_spinlock_lock(&intr_lock);

	/* check if the interrupt source for the fd is existent */
	TAILQ_FOREACH(src, &intr_sources, next)
		if (rte_intr_fd_get(src->intr_handle) == rte_intr_fd_get(intr_handle))
			break;

	/* No interrupt source registered for the fd */
	if (src == NULL) {
		ret = -ENOENT;

	/* interrupt source has some active callbacks right now. */
	} else if (src->active != 0) {
		ret = -EAGAIN;

	/* ok to remove. */
	} else {
		ret = 0;

		/*walk through the callbacks and remove all that match. */
		for (cb = TAILQ_FIRST(&src->callbacks); cb != NULL; cb = next) {

			next = TAILQ_NEXT(cb, next);

			if (cb->cb_fn == cb_fn && (cb_arg == (void *)-1 ||
					cb->cb_arg == cb_arg)) {
				TAILQ_REMOVE(&src->callbacks, cb, next);
				free(cb);
				ret++;
			}
		}

		/* all callbacks for that source are removed. */
		if (TAILQ_EMPTY(&src->callbacks)) {
			TAILQ_REMOVE(&intr_sources, src, next);
			rte_intr_instance_free(src->intr_handle);
			free(src);
		}
	}

	rte_spinlock_unlock(&intr_lock);

	/* notify the pipe fd waited by epoll_wait to rebuild the wait list */
	if (ret >= 0 && write(intr_pipe.writefd, "1", 1) < 0) {
		ret = -EPIPE;
	}

	rte_eal_trace_intr_callback_unregister(intr_handle, cb_fn, cb_arg,
		ret);
	return ret;
}

int
rte_intr_callback_unregister_sync(const struct rte_intr_handle *intr_handle,
			rte_intr_callback_fn cb_fn, void *cb_arg)
{
	int ret = 0;

	while ((ret = rte_intr_callback_unregister(intr_handle, cb_fn, cb_arg)) == -EAGAIN)
		rte_pause();

	return ret;
}

int
rte_intr_enable(const struct rte_intr_handle *intr_handle)
{
	int rc = 0, uio_cfg_fd;

	if (intr_handle == NULL)
		return -1;

	if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VDEV) {
		rc = 0;
		goto out;
	}

	uio_cfg_fd = rte_intr_dev_fd_get(intr_handle);
	if (rte_intr_fd_get(intr_handle) < 0 || uio_cfg_fd < 0) {
		rc = -1;
		goto out;
	}

	switch (rte_intr_type_get(intr_handle)) {
	/* write to the uio fd to enable the interrupt */
	case RTE_INTR_HANDLE_UIO:
		if (uio_intr_enable(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_UIO_INTX:
		if (uio_intx_intr_enable(intr_handle))
			rc = -1;
		break;
	/* not used at this moment */
	case RTE_INTR_HANDLE_ALARM:
		rc = -1;
		break;
#ifdef VFIO_PRESENT
	case RTE_INTR_HANDLE_VFIO_MSIX:
		if (vfio_enable_msix(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_VFIO_MSI:
		if (vfio_enable_msi(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_VFIO_LEGACY:
		if (vfio_enable_intx(intr_handle))
			rc = -1;
		break;
#ifdef HAVE_VFIO_DEV_REQ_INTERFACE
	case RTE_INTR_HANDLE_VFIO_REQ:
		if (vfio_enable_req(intr_handle))
			rc = -1;
		break;
#endif
#endif
	/* not used at this moment */
	case RTE_INTR_HANDLE_DEV_EVENT:
		rc = -1;
		break;
	/* unknown handle type */
	default:
		EAL_LOG(ERR, "Unknown handle type of fd %d",
			rte_intr_fd_get(intr_handle));
		rc = -1;
		break;
	}
out:
	rte_eal_trace_intr_enable(intr_handle, rc);
	return rc;
}

int
rte_intr_enable_uintr(const struct rte_intr_handle *intr_handle)
{
	int rc = 0, uio_cfg_fd;

	if (intr_handle == NULL)
		return -1;

	if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VDEV) {
		rc = 0;
		goto out;
	}

	uio_cfg_fd = rte_intr_dev_fd_get(intr_handle);
	if (rte_intr_fd_get(intr_handle) < 0 || uio_cfg_fd < 0) {
		rc = -1;
		goto out;
	}

	switch (rte_intr_type_get(intr_handle)) {
	/* write to the uio fd to enable the interrupt */
	case RTE_INTR_HANDLE_UIO:
		if (uio_intr_enable(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_UIO_INTX:
		if (uio_intx_intr_enable(intr_handle))
			rc = -1;
		break;
	/* not used at this moment */
	case RTE_INTR_HANDLE_ALARM:
		rc = -1;
		break;
#ifdef VFIO_PRESENT
	case RTE_INTR_HANDLE_VFIO_MSIX:
		if (vfio_enable_msix_uintr(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_VFIO_MSI:
		if (vfio_enable_msi(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_VFIO_LEGACY:
		if (vfio_enable_intx(intr_handle))
			rc = -1;
		break;
#ifdef HAVE_VFIO_DEV_REQ_INTERFACE
	case RTE_INTR_HANDLE_VFIO_REQ:
		if (vfio_enable_req(intr_handle))
			rc = -1;
		break;
#endif
#endif
	/* not used at this moment */
	case RTE_INTR_HANDLE_DEV_EVENT:
		rc = -1;
		break;
	/* unknown handle type */
	default:
		EAL_LOG(ERR, "Unknown handle type of fd %d",
			rte_intr_fd_get(intr_handle));
		rc = -1;
		break;
	}
out:
	rte_eal_trace_intr_enable(intr_handle, rc);
	return rc;
}

/**
 * PMD generally calls this function at the end of its IRQ callback.
 * Internally, it unmasks the interrupt if possible.
 *
 * For INTx, unmasking is required as the interrupt is auto-masked prior to
 * invoking callback.
 *
 * For MSI/MSI-X, unmasking is typically not needed as the interrupt is not
 * auto-masked. In fact, for interrupt handle types VFIO_MSIX and VFIO_MSI,
 * this function is no-op.
 */
int
rte_intr_ack(const struct rte_intr_handle *intr_handle)
{
	int uio_cfg_fd;

	if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VDEV)
		return 0;

	uio_cfg_fd = rte_intr_dev_fd_get(intr_handle);
	if (rte_intr_fd_get(intr_handle) < 0 || uio_cfg_fd < 0)
		return -1;

	switch (rte_intr_type_get(intr_handle)) {
	/* Both acking and enabling are same for UIO */
	case RTE_INTR_HANDLE_UIO:
		if (uio_intr_enable(intr_handle))
			return -1;
		break;
	case RTE_INTR_HANDLE_UIO_INTX:
		if (uio_intx_intr_enable(intr_handle))
			return -1;
		break;
	/* not used at this moment */
	case RTE_INTR_HANDLE_ALARM:
		return -1;
#ifdef VFIO_PRESENT
	/* VFIO MSI* is implicitly acked unlike INTx, nothing to do */
	case RTE_INTR_HANDLE_VFIO_MSIX:
	case RTE_INTR_HANDLE_VFIO_MSI:
		return 0;
	case RTE_INTR_HANDLE_VFIO_LEGACY:
		if (vfio_ack_intx(intr_handle))
			return -1;
		break;
#ifdef HAVE_VFIO_DEV_REQ_INTERFACE
	case RTE_INTR_HANDLE_VFIO_REQ:
		return -1;
#endif
#endif
	/* not used at this moment */
	case RTE_INTR_HANDLE_DEV_EVENT:
		return -1;
	/* unknown handle type */
	default:
		EAL_LOG(ERR, "Unknown handle type of fd %d",
			rte_intr_fd_get(intr_handle));
		return -1;
	}

	return 0;
}

int
rte_intr_disable(const struct rte_intr_handle *intr_handle)
{
	int rc = 0, uio_cfg_fd;

	if (intr_handle == NULL)
		return -1;

	if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VDEV) {
		rc = 0;
		goto out;
	}

	uio_cfg_fd = rte_intr_dev_fd_get(intr_handle);
	if (rte_intr_fd_get(intr_handle) < 0 || uio_cfg_fd < 0) {
		rc = -1;
		goto out;
	}

	switch (rte_intr_type_get(intr_handle)) {
	/* write to the uio fd to disable the interrupt */
	case RTE_INTR_HANDLE_UIO:
		if (uio_intr_disable(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_UIO_INTX:
		if (uio_intx_intr_disable(intr_handle))
			rc = -1;
		break;
	/* not used at this moment */
	case RTE_INTR_HANDLE_ALARM:
		rc = -1;
		break;
#ifdef VFIO_PRESENT
	case RTE_INTR_HANDLE_VFIO_MSIX:
		if (vfio_disable_msix(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_VFIO_MSI:
		if (vfio_disable_msi(intr_handle))
			rc = -1;
		break;
	case RTE_INTR_HANDLE_VFIO_LEGACY:
		if (vfio_disable_intx(intr_handle))
			rc = -1;
		break;
#ifdef HAVE_VFIO_DEV_REQ_INTERFACE
	case RTE_INTR_HANDLE_VFIO_REQ:
		if (vfio_disable_req(intr_handle))
			rc = -1;
		break;
#endif
#endif
	/* not used at this moment */
	case RTE_INTR_HANDLE_DEV_EVENT:
		rc = -1;
		break;
	/* unknown handle type */
	default:
		EAL_LOG(ERR, "Unknown handle type of fd %d",
			rte_intr_fd_get(intr_handle));
		rc = -1;
		break;
	}
out:
	rte_eal_trace_intr_disable(intr_handle, rc);
	return rc;
}

static int
eal_intr_process_interrupts(struct epoll_event *events, int nfds)
{
	bool call = false;
	int n, bytes_read, rv;
	struct rte_intr_source *src;
	struct rte_intr_callback *cb, *next;
	union rte_intr_read_buffer buf;
	struct rte_intr_callback active_cb;

	for (n = 0; n < nfds; n++) {

		/**
		 * if the pipe fd is ready to read, return out to
		 * rebuild the wait list.
		 */
		if (events[n].data.fd == intr_pipe.readfd){
			int r = read(intr_pipe.readfd, buf.charbuf,
					sizeof(buf.charbuf));
			RTE_SET_USED(r);
			return -1;
		}
		rte_spinlock_lock(&intr_lock);
		TAILQ_FOREACH(src, &intr_sources, next)
			if (rte_intr_fd_get(src->intr_handle) == events[n].data.fd)
				break;
		if (src == NULL){
			rte_spinlock_unlock(&intr_lock);
			continue;
		}

		/* mark this interrupt source as active and release the lock. */
		src->active = 1;
		rte_spinlock_unlock(&intr_lock);

		/* set the length to be read dor different handle type */
		switch (rte_intr_type_get(src->intr_handle)) {
		case RTE_INTR_HANDLE_UIO:
		case RTE_INTR_HANDLE_UIO_INTX:
			bytes_read = sizeof(buf.uio_intr_count);
			break;
		case RTE_INTR_HANDLE_ALARM:
			bytes_read = sizeof(buf.timerfd_num);
			break;
#ifdef VFIO_PRESENT
#ifdef HAVE_VFIO_DEV_REQ_INTERFACE
		case RTE_INTR_HANDLE_VFIO_REQ:
#endif
		case RTE_INTR_HANDLE_VFIO_MSIX:
		case RTE_INTR_HANDLE_VFIO_MSI:
		case RTE_INTR_HANDLE_VFIO_LEGACY:
			bytes_read = sizeof(buf.vfio_intr_count);
			break;
#endif
		case RTE_INTR_HANDLE_VDEV:
		case RTE_INTR_HANDLE_EXT:
			bytes_read = 0;
			call = true;
			break;
		case RTE_INTR_HANDLE_DEV_EVENT:
			bytes_read = 0;
			call = true;
			break;
		default:
			bytes_read = 1;
			break;
		}

		if (bytes_read > 0) {
			/**
			 * read out to clear the ready-to-be-read flag
			 * for epoll_wait.
			 */
			bytes_read = read(events[n].data.fd, &buf, bytes_read);
			if (bytes_read < 0) {
				if (errno == EINTR || errno == EWOULDBLOCK)
					continue;

				EAL_LOG(ERR, "Error reading from file "
					"descriptor %d: %s",
					events[n].data.fd,
					strerror(errno));
				/*
				 * The device is unplugged or buggy, remove
				 * it as an interrupt source and return to
				 * force the wait list to be rebuilt.
				 */
				rte_spinlock_lock(&intr_lock);
				TAILQ_REMOVE(&intr_sources, src, next);
				rte_spinlock_unlock(&intr_lock);

				for (cb = TAILQ_FIRST(&src->callbacks); cb;
							cb = next) {
					next = TAILQ_NEXT(cb, next);
					TAILQ_REMOVE(&src->callbacks, cb, next);
					free(cb);
				}
				rte_intr_instance_free(src->intr_handle);
				free(src);
				return -1;
			} else if (bytes_read == 0)
				EAL_LOG(ERR, "Read nothing from file "
					"descriptor %d", events[n].data.fd);
			else
				call = true;
		}

		/* grab a lock, again to call callbacks and update status. */
		rte_spinlock_lock(&intr_lock);

		if (call) {

			/* Finally, call all callbacks. */
			TAILQ_FOREACH(cb, &src->callbacks, next) {

				/* make a copy and unlock. */
				active_cb = *cb;
				rte_spinlock_unlock(&intr_lock);

				/* call the actual callback */
				active_cb.cb_fn(active_cb.cb_arg);

				/*get the lock back. */
				rte_spinlock_lock(&intr_lock);
			}
		}
		/* we done with that interrupt source, release it. */
		src->active = 0;

		rv = 0;

		/* check if any callback are supposed to be removed */
		for (cb = TAILQ_FIRST(&src->callbacks); cb != NULL; cb = next) {
			next = TAILQ_NEXT(cb, next);
			if (cb->pending_delete) {
				TAILQ_REMOVE(&src->callbacks, cb, next);
				if (cb->ucb_fn)
					cb->ucb_fn(src->intr_handle, cb->cb_arg);
				free(cb);
				rv++;
			}
		}

		/* all callbacks for that source are removed. */
		if (TAILQ_EMPTY(&src->callbacks)) {
			TAILQ_REMOVE(&intr_sources, src, next);
			rte_intr_instance_free(src->intr_handle);
			free(src);
		}

		/* notify the pipe fd waited by epoll_wait to rebuild the wait list */
		if (rv > 0 && write(intr_pipe.writefd, "1", 1) < 0) {
			rte_spinlock_unlock(&intr_lock);
			return -EPIPE;
		}

		rte_spinlock_unlock(&intr_lock);
	}

	return 0;
}

/**
 * It handles all the interrupts.
 *
 * @param pfd
 *  epoll file descriptor.
 * @param totalfds
 *  The number of file descriptors added in epoll.
 *
 * @return
 *  void
 */
static void
eal_intr_handle_interrupts(int pfd, unsigned totalfds)
{
	struct epoll_event events[totalfds];
	int nfds = 0;

	for(;;) {
		nfds = epoll_wait(pfd, events, totalfds,
			EAL_INTR_EPOLL_WAIT_FOREVER);
		/* epoll_wait fail */
		if (nfds < 0) {
			if (errno == EINTR)
				continue;
			EAL_LOG(ERR,
				"epoll_wait returns with fail");
			return;
		}
		/* epoll_wait timeout, will never happens here */
		else if (nfds == 0)
			continue;
		/* epoll_wait has at least one fd ready to read */
		if (eal_intr_process_interrupts(events, nfds) < 0)
			return;
	}
}

/**
 * It builds/rebuilds up the epoll file descriptor with all the
 * file descriptors being waited on. Then handles the interrupts.
 *
 * @param arg
 *  pointer. (unused)
 *
 * @return
 *  never return;
 */
static __rte_noreturn uint32_t
eal_intr_thread_main(__rte_unused void *arg)
{
	/* host thread, never break out */
	for (;;) {
		/* build up the epoll fd with all descriptors we are to
		 * wait on then pass it to the handle_interrupts function
		 */
		static struct epoll_event pipe_event = {
			.events = EPOLLIN | EPOLLPRI,
		};
		struct rte_intr_source *src;
		unsigned numfds = 0;

		/* create epoll fd */
		int pfd = epoll_create(1);
		if (pfd < 0)
			rte_panic("Cannot create epoll instance\n");

		pipe_event.data.fd = intr_pipe.readfd;
		/**
		 * add pipe fd into wait list, this pipe is used to
		 * rebuild the wait list.
		 */
		if (epoll_ctl(pfd, EPOLL_CTL_ADD, intr_pipe.readfd,
						&pipe_event) < 0) {
			rte_panic("Error adding fd to %d epoll_ctl, %s\n",
					intr_pipe.readfd, strerror(errno));
		}
		numfds++;

		rte_spinlock_lock(&intr_lock);

		TAILQ_FOREACH(src, &intr_sources, next) {
			struct epoll_event ev;

			if (src->callbacks.tqh_first == NULL)
				continue; /* skip those with no callbacks */
			memset(&ev, 0, sizeof(ev));
			ev.events = EPOLLIN | EPOLLPRI | EPOLLRDHUP | EPOLLHUP;
			ev.data.fd = rte_intr_fd_get(src->intr_handle);

			/**
			 * add all the uio device file descriptor
			 * into wait list.
			 */
			if (epoll_ctl(pfd, EPOLL_CTL_ADD,
					rte_intr_fd_get(src->intr_handle), &ev) < 0) {
				rte_panic("Error adding fd %d epoll_ctl, %s\n",
					rte_intr_fd_get(src->intr_handle),
					strerror(errno));
			}
			else
				numfds++;
		}
		rte_spinlock_unlock(&intr_lock);
		/* serve the interrupt */
		eal_intr_handle_interrupts(pfd, numfds);

		/**
		 * when we return, we need to rebuild the
		 * list of fds to monitor.
		 */
		close(pfd);
	}
}

int
rte_eal_intr_init(void)
{
	int ret = 0;

	/* init the global interrupt source head */
	TAILQ_INIT(&intr_sources);

	/**
	 * create a pipe which will be waited by epoll and notified to
	 * rebuild the wait list of epoll.
	 */
	if (pipe(intr_pipe.pipefd) < 0) {
		rte_errno = errno;
		return -1;
	}

	/* create the host thread to wait/handle the interrupt */
	ret = rte_thread_create_internal_control(&intr_thread, "intr",
			eal_intr_thread_main, NULL);
	if (ret != 0) {
		rte_errno = -ret;
		EAL_LOG(ERR,
			"Failed to create thread for interrupt handling");
	}

	return ret;
}

static void
eal_intr_proc_rxtx_intr(int fd, const struct rte_intr_handle *intr_handle)
{
	union rte_intr_read_buffer buf;
	int bytes_read = 0;
	int nbytes;

	switch (rte_intr_type_get(intr_handle)) {
	case RTE_INTR_HANDLE_UIO:
	case RTE_INTR_HANDLE_UIO_INTX:
		bytes_read = sizeof(buf.uio_intr_count);
		break;
#ifdef VFIO_PRESENT
	case RTE_INTR_HANDLE_VFIO_MSIX:
	case RTE_INTR_HANDLE_VFIO_MSI:
	case RTE_INTR_HANDLE_VFIO_LEGACY:
		bytes_read = sizeof(buf.vfio_intr_count);
		break;
#endif
	case RTE_INTR_HANDLE_VDEV:
		bytes_read = rte_intr_efd_counter_size_get(intr_handle);
		/* For vdev, number of bytes to read is set by driver */
		break;
	case RTE_INTR_HANDLE_EXT:
		return;
	default:
		bytes_read = 1;
		EAL_LOG(INFO, "unexpected intr type");
		break;
	}

	/**
	 * read out to clear the ready-to-be-read flag
	 * for epoll_wait.
	 */
	if (bytes_read == 0)
		return;
	do {
		nbytes = read(fd, &buf, bytes_read);
		if (nbytes < 0) {
			if (errno == EINTR || errno == EWOULDBLOCK ||
			    errno == EAGAIN)
				continue;
			EAL_LOG(ERR,
				"Error reading from fd %d: %s",
				fd, strerror(errno));
		} else if (nbytes == 0)
			EAL_LOG(ERR, "Read nothing from fd %d", fd);
		return;
	} while (1);
}

static int
eal_epoll_process_event(struct epoll_event *evs, unsigned int n,
			struct rte_epoll_event *events)
{
	unsigned int i, count = 0;
	struct rte_epoll_event *rev;
	uint32_t valid_status;

	for (i = 0; i < n; i++) {
		rev = evs[i].data.ptr;
		valid_status =  RTE_EPOLL_VALID;
		/* ACQUIRE memory ordering here pairs with RELEASE
		 * ordering below acting as a lock to synchronize
		 * the event data updating.
		 */
		if (!rev || !rte_atomic_compare_exchange_strong_explicit(&rev->status,
				    &valid_status, RTE_EPOLL_EXEC,
				    rte_memory_order_acquire, rte_memory_order_relaxed))
			continue;

		events[count].status        = RTE_EPOLL_VALID;
		events[count].fd            = rev->fd;
		events[count].epfd          = rev->epfd;
		events[count].epdata.event  = evs[i].events;
		events[count].epdata.data   = rev->epdata.data;
		if (rev->epdata.cb_fun)
			rev->epdata.cb_fun(rev->fd,
					   rev->epdata.cb_arg);

		/* the status update should be observed after
		 * the other fields change.
		 */
		rte_atomic_store_explicit(&rev->status, RTE_EPOLL_VALID,
				rte_memory_order_release);
		count++;
	}
	return count;
}

static inline int
eal_init_tls_epfd(void)
{
	int pfd = epoll_create(255);

	if (pfd < 0) {
		EAL_LOG(ERR,
			"Cannot create epoll instance");
		return -1;
	}
	return pfd;
}

int
rte_intr_tls_epfd(void)
{
	if (RTE_PER_LCORE(_epfd) == -1)
		RTE_PER_LCORE(_epfd) = eal_init_tls_epfd();

	return RTE_PER_LCORE(_epfd);
}

static int
eal_epoll_wait(int epfd, struct rte_epoll_event *events,
	       int maxevents, int timeout, bool interruptible)
{
	struct epoll_event evs[maxevents];
	int rc;

	if (!events) {
		EAL_LOG(ERR, "rte_epoll_event can't be NULL");
		return -1;
	}

	/* using per thread epoll fd */
	if (epfd == RTE_EPOLL_PER_THREAD)
		epfd = rte_intr_tls_epfd();

	while (1) {
		rc = epoll_wait(epfd, evs, maxevents, timeout);
		if (likely(rc > 0)) {
			/* epoll_wait has at least one fd ready to read */
			rc = eal_epoll_process_event(evs, rc, events);
			break;
		} else if (rc < 0) {
			if (errno == EINTR) {
				if (interruptible)
					return -1;
				else
					continue;
			}
			/* epoll_wait fail */
			EAL_LOG(ERR, "epoll_wait returns with fail %s",
				strerror(errno));
			rc = -1;
			break;
		} else {
			/* rc == 0, epoll_wait timed out */
			break;
		}
	}

	return rc;
}

int
rte_epoll_wait(int epfd, struct rte_epoll_event *events,
	       int maxevents, int timeout)
{
	return eal_epoll_wait(epfd, events, maxevents, timeout, false);
}

int
rte_epoll_wait_interruptible(int epfd, struct rte_epoll_event *events,
			     int maxevents, int timeout)
{
	return eal_epoll_wait(epfd, events, maxevents, timeout, true);
}

static inline void
eal_epoll_data_safe_free(struct rte_epoll_event *ev)
{
	uint32_t valid_status = RTE_EPOLL_VALID;

	while (!rte_atomic_compare_exchange_strong_explicit(&ev->status, &valid_status,
		    RTE_EPOLL_INVALID, rte_memory_order_acquire, rte_memory_order_relaxed)) {
		while (rte_atomic_load_explicit(&ev->status,
				rte_memory_order_relaxed) != RTE_EPOLL_VALID)
			rte_pause();
		valid_status = RTE_EPOLL_VALID;
	}
	memset(&ev->epdata, 0, sizeof(ev->epdata));
	ev->fd = -1;
	ev->epfd = -1;
}

int
rte_epoll_ctl(int epfd, int op, int fd,
	      struct rte_epoll_event *event)
{
	struct epoll_event ev;

	if (!event) {
		EAL_LOG(ERR, "rte_epoll_event can't be NULL");
		return -1;
	}

	/* using per thread epoll fd */
	if (epfd == RTE_EPOLL_PER_THREAD)
		epfd = rte_intr_tls_epfd();

	if (op == EPOLL_CTL_ADD) {
		rte_atomic_store_explicit(&event->status, RTE_EPOLL_VALID,
				rte_memory_order_relaxed);
		event->fd = fd;  /* ignore fd in event */
		event->epfd = epfd;
		ev.data.ptr = (void *)event;
	}

	ev.events = event->epdata.event;
	if (epoll_ctl(epfd, op, fd, &ev) < 0) {
		EAL_LOG(ERR, "Error op %d fd %d epoll_ctl, %s",
			op, fd, strerror(errno));
		if (op == EPOLL_CTL_ADD)
			/* rollback status when CTL_ADD fail */
			rte_atomic_store_explicit(&event->status, RTE_EPOLL_INVALID,
					rte_memory_order_relaxed);
		return -1;
	}

	if (op == EPOLL_CTL_DEL && rte_atomic_load_explicit(&event->status,
			rte_memory_order_relaxed) != RTE_EPOLL_INVALID)
		eal_epoll_data_safe_free(event);

	return 0;
}

int
rte_intr_rx_ctl(struct rte_intr_handle *intr_handle, int epfd,
		int op, unsigned int vec, void *data)
{
	struct rte_epoll_event *rev;
	struct rte_epoll_data *epdata;
	int epfd_op;
	unsigned int efd_idx;
	int rc = 0;

	efd_idx = (vec >= RTE_INTR_VEC_RXTX_OFFSET) ?
		(vec - RTE_INTR_VEC_RXTX_OFFSET) : vec;

	if (intr_handle == NULL || rte_intr_nb_efd_get(intr_handle) == 0 ||
			efd_idx >= (unsigned int)rte_intr_nb_efd_get(intr_handle)) {
		EAL_LOG(ERR, "Wrong intr vector number.");
		return -EPERM;
	}

	switch (op) {
	case RTE_INTR_EVENT_ADD:
		epfd_op = EPOLL_CTL_ADD;
		rev = rte_intr_elist_index_get(intr_handle, efd_idx);
		if (rte_atomic_load_explicit(&rev->status,
				rte_memory_order_relaxed) != RTE_EPOLL_INVALID) {
			EAL_LOG(INFO, "Event already been added.");
			return -EEXIST;
		}

		/* attach to intr vector fd */
		epdata = &rev->epdata;
		epdata->event  = EPOLLIN | EPOLLPRI | EPOLLET;
		epdata->data   = data;
		epdata->cb_fun = (rte_intr_event_cb_t)eal_intr_proc_rxtx_intr;
		epdata->cb_arg = (void *)intr_handle;
		rc = rte_epoll_ctl(epfd, epfd_op,
			rte_intr_efds_index_get(intr_handle, efd_idx), rev);
		if (!rc)
			EAL_LOG(DEBUG,
				"efd %d associated with vec %d added on epfd %d",
				rev->fd, vec, epfd);
		else
			rc = -EPERM;
		break;
	case RTE_INTR_EVENT_DEL:
		epfd_op = EPOLL_CTL_DEL;
		rev = rte_intr_elist_index_get(intr_handle, efd_idx);
		if (rte_atomic_load_explicit(&rev->status,
				rte_memory_order_relaxed) == RTE_EPOLL_INVALID) {
			EAL_LOG(INFO, "Event does not exist.");
			return -EPERM;
		}

		rc = rte_epoll_ctl(rev->epfd, epfd_op, rev->fd, rev);
		if (rc)
			rc = -EPERM;
		break;
	default:
		EAL_LOG(ERR, "event op type mismatch");
		rc = -EPERM;
	}

	return rc;
}

void
rte_intr_free_epoll_fd(struct rte_intr_handle *intr_handle)
{
	uint32_t i;
	struct rte_epoll_event *rev;

	for (i = 0; i < (uint32_t)rte_intr_nb_efd_get(intr_handle); i++) {
		rev = rte_intr_elist_index_get(intr_handle, i);
		if (rte_atomic_load_explicit(&rev->status,
				rte_memory_order_relaxed) == RTE_EPOLL_INVALID)
			continue;
		if (rte_epoll_ctl(rev->epfd, EPOLL_CTL_DEL, rev->fd, rev)) {
			/* force free if the entry valid */
			eal_epoll_data_safe_free(rev);
		}
	}
}

int
rte_intr_efd_enable(struct rte_intr_handle *intr_handle, uint32_t nb_efd)
{
	uint32_t i;
	int fd;
	uint32_t n = RTE_MIN(nb_efd, (uint32_t)RTE_MAX_RXTX_INTR_VEC_ID);

	assert(nb_efd != 0);

	if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VFIO_MSIX) {
		for (i = 0; i < n; i++) {
			fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
			if (fd < 0) {
				EAL_LOG(ERR,
					"can't setup eventfd, error %i (%s)",
					errno, strerror(errno));
				return -errno;
			}

			if (rte_intr_efds_index_set(intr_handle, i, fd))
				return -rte_errno;
		}

		if (rte_intr_nb_efd_set(intr_handle, n))
			return -rte_errno;

		if (rte_intr_max_intr_set(intr_handle, NB_OTHER_INTR + n))
			return -rte_errno;
	} else if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VDEV) {
		/* only check, initialization would be done in vdev driver.*/
		if ((uint64_t)rte_intr_efd_counter_size_get(intr_handle) >
		    sizeof(union rte_intr_read_buffer)) {
			EAL_LOG(ERR, "the efd_counter_size is oversized");
			return -EINVAL;
		}
	} else {
		if (rte_intr_efds_index_set(intr_handle, 0, rte_intr_fd_get(intr_handle)))
			return -rte_errno;
		if (rte_intr_nb_efd_set(intr_handle, RTE_MIN(nb_efd, 1U)))
			return -rte_errno;
		if (rte_intr_max_intr_set(intr_handle, NB_OTHER_INTR))
			return -rte_errno;
	}

	return 0;
}

int
rte_intr_efd_enable_uintr(struct rte_intr_handle *intr_handle, uint32_t nb_efd)
{
	uint32_t i;
	int fd;
	uint32_t n = RTE_MIN(nb_efd, (uint32_t)RTE_MAX_RXTX_INTR_VEC_ID);

	assert(nb_efd != 0);

	// #define UINTR_HANDLER_FLAG_WAITING_RECEIVER	0x1000 // TODO: 这个定义也一直需要吗？
	// if (uintr_register_handler(uintr_handler, UINTR_HANDLER_FLAG_WAITING_RECEIVER)) {
	// 	EAL_LOG(ERR,"[FAIL]\tInterrupt handler register error");
	// 	exit(EXIT_FAILURE);
	// }
	static int temp = 0; // TODO: 临时处理
	temp++;

	if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VFIO_MSIX) {
		for (i = 0; i < n; i++) {
			fd = uintr_create_fd(temp + n, 0);
			EAL_LOG(ERR, "uintr fd %d", fd);
			_stui();
			if (fd < 0) {
				EAL_LOG(ERR,
					"can't setup eventfd, error %i (%s)",
					errno, strerror(errno));
				return -errno;
			}

			if (rte_intr_efds_index_set(intr_handle, i, fd))
				return -rte_errno;
		}

		if (rte_intr_nb_efd_set(intr_handle, n))
			return -rte_errno;

		if (rte_intr_max_intr_set(intr_handle, NB_OTHER_INTR + n))
			return -rte_errno;
	} else if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VDEV) {
		/* only check, initialization would be done in vdev driver.*/
		if ((uint64_t)rte_intr_efd_counter_size_get(intr_handle) >
		    sizeof(union rte_intr_read_buffer)) {
			EAL_LOG(ERR, "the efd_counter_size is oversized");
			return -EINVAL;
		}
	} else {
		if (rte_intr_efds_index_set(intr_handle, 0, rte_intr_fd_get(intr_handle)))
			return -rte_errno;
		if (rte_intr_nb_efd_set(intr_handle, RTE_MIN(nb_efd, 1U)))
			return -rte_errno;
		if (rte_intr_max_intr_set(intr_handle, NB_OTHER_INTR))
			return -rte_errno;
	}

	return 0;
}

void
rte_intr_efd_disable(struct rte_intr_handle *intr_handle)
{
	uint32_t i;

	rte_intr_free_epoll_fd(intr_handle);
	if (rte_intr_max_intr_get(intr_handle) > rte_intr_nb_efd_get(intr_handle)) {
		for (i = 0; i < (uint32_t)rte_intr_nb_efd_get(intr_handle); i++)
			close(rte_intr_efds_index_get(intr_handle, i));
	}
	uintr_unregister_handler(0);
	rte_intr_nb_efd_set(intr_handle, 0);
	rte_intr_max_intr_set(intr_handle, 0);
}

int
rte_intr_dp_is_en(struct rte_intr_handle *intr_handle)
{
	return !(!rte_intr_nb_efd_get(intr_handle));
}

int
rte_intr_allow_others(struct rte_intr_handle *intr_handle)
{
	if (!rte_intr_dp_is_en(intr_handle))
		return 1;
	else
		return !!(rte_intr_max_intr_get(intr_handle) -
				rte_intr_nb_efd_get(intr_handle));
}

int
rte_intr_cap_multiple(struct rte_intr_handle *intr_handle)
{
	if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VFIO_MSIX)
		return 1;

	if (rte_intr_type_get(intr_handle) == RTE_INTR_HANDLE_VDEV)
		return 1;

	return 0;
}

int rte_thread_is_intr(void)
{
	return rte_thread_equal(intr_thread, rte_thread_self());
}
