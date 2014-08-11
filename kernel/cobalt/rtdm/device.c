/*
 * Real-Time Driver Model for Xenomai, device management
 *
 * Copyright (C) 2005 Jan Kiszka <jan.kiszka@web.de>
 * Copyright (C) 2005 Joerg Langenberg <joerg.langenberg@gmx.net>
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <cobalt/kernel/apc.h>
#include "rtdm/internal.h"
#include <trace/events/cobalt-rtdm.h>

/**
 * @addtogroup rtdm_driver_interface
 * @{
 */

#define RTDM_DEVICE_MAGIC	0x82846877

#define SET_DEFAULT_DUAL_OP_IF_NULL(device, operation, handler)		\
	if ((device).operation##_rt == NULL)				\
		(device).operation##_rt =				\
		(__typeof__((device).operation##_rt))handler;		\
	if ((device).operation##_nrt == NULL)				\
		(device).operation##_nrt =				\
		(__typeof__((device).operation##_nrt))handler;

#define SET_DEFAULT_OP_IF_NULL(device, operation, handler)		\
	if ((device).operation == NULL)					\
		(device).operation =					\
		(__typeof__((device).operation))handler;

struct list_head rtdm_named_devices;	/* hash table */
struct rb_root rtdm_protocol_devices;

struct semaphore nrt_dev_lock;
DEFINE_XNLOCK(rt_dev_lock);

int rtdm_initialised = 0;

extern void __rt_dev_close(struct rtdm_fd *fd);

static int enosys(void)
{
	return -ENOSYS;
}

static int ebadf(void)
{
	return -EBADF;
}

static int enodev(void)
{
	return -ENODEV;
}

static inline unsigned long long get_proto_id(int pf, int type)
{
	unsigned long long llpf = (unsigned)pf;
	return (llpf << 32) | (unsigned)type;
}

static inline void rtdm_reference_device(struct rtdm_device *device)
{
	atomic_inc(&device->reserved.refcount);
}

struct rtdm_device *get_named_device(const char *name)
{
	struct rtdm_device *device;
	xnhandle_t handle;
	int err;
	spl_t s;

	err = xnregistry_bind(name, XN_NONBLOCK, XN_RELATIVE, &handle);
	if (err == -EWOULDBLOCK)
		return NULL;

	xnlock_get_irqsave(&rt_dev_lock, s);

	device = xnregistry_lookup(handle, NULL);
	if (device) {
		if (device->reserved.magic != RTDM_DEVICE_MAGIC)
			device = NULL;
		else
			rtdm_reference_device(device);
	}

	xnlock_put_irqrestore(&rt_dev_lock, s);

	return device;
}

struct rtdm_device *get_protocol_device(int protocol_family, int socket_type)
{
	struct rtdm_device *device;
	unsigned long long id;
	struct xnid *xnid;
	spl_t s;

	id = get_proto_id(protocol_family, socket_type);

	xnlock_get_irqsave(&rt_dev_lock, s);

	xnid = xnid_fetch(&rtdm_protocol_devices, id);
	if (xnid) {
		device = container_of(xnid, struct rtdm_device, reserved.id);

		rtdm_reference_device(device);
	} else
		device = NULL;

	xnlock_put_irqrestore(&rt_dev_lock, s);

	return device;
}

/**
 * @ingroup rtdm_driver_interface
 * @defgroup rtdm_device_register Device Registration Services
 * @{
 */

/**
 * @brief Register a RTDM device
 *
 * @param[in] device Pointer to structure describing the new device.
 *
 * @return 0 is returned upon success. Otherwise:
 *
 * - -EINVAL is returned if the device structure contains invalid entries.
 * Check kernel log in this case.
 *
 * - -ENOMEM is returned if the context for an exclusive device cannot be
 * allocated.
 *
 * - -EEXIST is returned if the specified device name of protocol ID is
 * already in use.
 *
 * - -EAGAIN is returned if some /proc entry cannot be created.
 *
 * @coretags{secondary-only}
 */
int rtdm_dev_register(struct rtdm_device *device)
{
	unsigned long long id;
	spl_t s;
	int ret;

	/* Catch unsuccessful initialisation */
	if (!rtdm_initialised)
		return -ENOSYS;

	/* Sanity check: structure version */
	if (!XENO_ASSERT(RTDM, device->struct_version == RTDM_DEVICE_STRUCT_VER)) {
		printk(XENO_ERR "invalid rtdm_device version (%d, "
		       "required %d)\n", device->struct_version,
		       RTDM_DEVICE_STRUCT_VER);
		return -EINVAL;
	}

	/* Sanity check: proc_name specified? */
	if (!XENO_ASSERT(RTDM, device->proc_name)) {
		printk(XENO_ERR "no vfile (/proc) name specified for RTDM device\n");
		return -EINVAL;
	}

	switch (device->device_flags & RTDM_DEVICE_TYPE_MASK) {
	case RTDM_NAMED_DEVICE:
		/* Sanity check: any open handler? */
		if (device->open == NULL) {
			printk(XENO_ERR "missing open handler for RTDM device\n");
			return -EINVAL;
		}
		device->socket = (typeof(device->socket))enosys;
		break;

	case RTDM_PROTOCOL_DEVICE:
		/* Sanity check: any socket handler? */
		if (device->socket == NULL) {
			printk(XENO_ERR "missing socket handler for RTDM device\n");
			return -EINVAL;
		}
		device->open = (typeof(device->open))enosys;
		break;

	default:
		return -EINVAL;
	}

	/* Sanity check: non-RT close handler?
	 * (Always required for forced cleanup) */
	if (device->ops.close == NULL) {
		printk(XENO_ERR "missing close handler for RTDM device\n");
		return -EINVAL;
	}
	device->reserved.close = device->ops.close;
	device->ops.close = __rt_dev_close;

	SET_DEFAULT_DUAL_OP_IF_NULL(device->ops, ioctl, enosys);
	SET_DEFAULT_DUAL_OP_IF_NULL(device->ops, read, enosys);
	SET_DEFAULT_DUAL_OP_IF_NULL(device->ops, write, enosys);
	SET_DEFAULT_DUAL_OP_IF_NULL(device->ops, recvmsg, enosys);
	SET_DEFAULT_DUAL_OP_IF_NULL(device->ops, sendmsg, enosys);
	SET_DEFAULT_OP_IF_NULL(device->ops, select_bind, ebadf);
	SET_DEFAULT_OP_IF_NULL(device->ops, mmap, enodev);

	atomic_set(&device->reserved.refcount, 0);
	device->reserved.exclusive_context = NULL;

	if (device->device_flags & RTDM_EXCLUSIVE) {
		device->reserved.exclusive_context =
		    kmalloc(sizeof(struct rtdm_dev_context) +
			    device->context_size, GFP_KERNEL);
		if (!device->reserved.exclusive_context) {
			printk(XENO_ERR "no memory for exclusive context of RTDM device "
			       "(context size: %ld)\n",
				 (long)device->context_size);
			return -ENOMEM;
		}
		/* mark exclusive context as unused */
		device->reserved.exclusive_context->device = NULL;
	}

	down(&nrt_dev_lock);

	trace_cobalt_device_register(device);

	device->reserved.magic = RTDM_DEVICE_MAGIC;

	if ((device->device_flags & RTDM_DEVICE_TYPE_MASK) ==
		RTDM_NAMED_DEVICE) {
		ret = rtdm_proc_register_device(device);
		if (ret)
			goto err;

		ret = xnregistry_enter(device->device_name, device,
				&device->reserved.handle, NULL);
		if (ret) {
			rtdm_proc_unregister_device(device);
			goto err;
		}

		xnlock_get_irqsave(&rt_dev_lock, s);
		list_add_tail(&device->reserved.entry, &rtdm_named_devices);
		xnlock_put_irqrestore(&rt_dev_lock, s);

		up(&nrt_dev_lock);
	} else {
		id = get_proto_id(device->protocol_family,
				device->socket_type);

		xnlock_get_irqsave(&rt_dev_lock, s);
		ret = xnid_enter(&rtdm_protocol_devices,
				&device->reserved.id, id);
		xnlock_put_irqrestore(&rt_dev_lock, s);
		if (ret < 0)
			goto err;

		ret = rtdm_proc_register_device(device);
		if (ret) {
			xnlock_get_irqsave(&rt_dev_lock, s);
			xnid_remove(&rtdm_protocol_devices,
				&device->reserved.id);
			xnlock_put_irqrestore(&rt_dev_lock, s);
			goto err;
		}

		up(&nrt_dev_lock);
	}
	return 0;

err:
	up(&nrt_dev_lock);
	if (device->reserved.exclusive_context)
		kfree(device->reserved.exclusive_context);
	return ret;
}

EXPORT_SYMBOL_GPL(rtdm_dev_register);

/**
 * @brief Unregisters a RTDM device
 *
 * @param[in] device Pointer to structure describing the device to be
 * unregistered.
 * @param[in] poll_delay Polling delay in milliseconds to check repeatedly for
 * open instances of @a device, or 0 for non-blocking mode.
 *
 * @return 0 is returned upon success. Otherwise:
 *
 * - -EAGAIN is returned if the device is busy with open instances and
 * 0 has been passed for @a poll_delay.
 *
 * @coretags{secondary-only}
 */
int rtdm_dev_unregister(struct rtdm_device *device, unsigned int poll_delay)
{
	unsigned long warned = 0;
	xnhandle_t handle = 0;
	spl_t s;

	if (!rtdm_initialised)
		return -ENOSYS;

	rtdm_reference_device(device);

	trace_cobalt_device_unregister(device, poll_delay);

	down(&nrt_dev_lock);
	xnlock_get_irqsave(&rt_dev_lock, s);

	while (atomic_read(&device->reserved.refcount) > 1) {
		xnlock_put_irqrestore(&rt_dev_lock, s);
		up(&nrt_dev_lock);

		if (!poll_delay) {
			rtdm_dereference_device(device);
			return -EAGAIN;
		}

		if (!__test_and_set_bit(0, &warned))
			printk(XENO_WARN "RTDM device %s still in use - waiting for"
			       " release...\n", device->device_name);
		msleep(poll_delay);
		down(&nrt_dev_lock);
		xnlock_get_irqsave(&rt_dev_lock, s);
	}

	if ((device->device_flags & RTDM_DEVICE_TYPE_MASK) ==
		RTDM_NAMED_DEVICE) {
		handle = device->reserved.handle;
		list_del(&device->reserved.entry);
	} else
		xnid_remove(&rtdm_protocol_devices, &device->reserved.id);

	xnlock_put_irqrestore(&rt_dev_lock, s);

	rtdm_proc_unregister_device(device);

	if (handle)
		xnregistry_remove(handle);

	up(&nrt_dev_lock);

	if (device->reserved.exclusive_context)
		kfree(device->reserved.exclusive_context);

	return 0;
}

EXPORT_SYMBOL_GPL(rtdm_dev_unregister);
/** @} */

int __init rtdm_dev_init(void)
{
	sema_init(&nrt_dev_lock, 1);

	INIT_LIST_HEAD(&rtdm_named_devices);
	xntree_init(&rtdm_protocol_devices);

	return 0;
}

void rtdm_dev_cleanup(void)
{
	/*
	 * Note: no need to flush the cleanup_queue as no device is allowed
	 * to deregister as long as there are references.
	 */
}

/*@}*/
