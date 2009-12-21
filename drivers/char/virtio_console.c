/*
 * Copyright (C) 2006, 2007, 2009 Rusty Russell, IBM Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <linux/err.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/virtio.h>
#include <linux/virtio_console.h>
#include "hvc_console.h"

/*
 * This is a global struct for storing common data for all the devices
 * this driver handles.
 *
 * Mainly, it has a linked list for all the consoles in one place so
 * that callbacks from hvc for get_chars(), put_chars() work properly
 * across multiple devices and multiple ports per device.
 */
struct ports_driver_data {
	/*
	 * This is used to keep track of the number of hvc consoles
	 * spawned by this driver.  This number is given as the first
	 * argument to hvc_alloc().  To correctly map an initial
	 * console spawned via hvc_instantiate to the console being
	 * hooked up via hvc_alloc, we need to pass the same vtermno.
	 *
	 * We also just assume the first console being initialised was
	 * the first one that got used as the initial console.
	 */
	unsigned int next_vtermno;

	/* All the console devices handled by this driver */
	struct list_head consoles;
};
static struct ports_driver_data pdrvdata;

DEFINE_SPINLOCK(pdrvdata_lock);

/* This struct holds information that's relevant only for console ports */
struct console {
	/* We'll place all consoles in a list in the pdrvdata struct */
	struct list_head list;

	/* The hvc device associated with this console port */
	struct hvc_struct *hvc;

	/*
	 * This number identifies the number that we used to register
	 * with hvc in hvc_instantiate() and hvc_alloc(); this is the
	 * number passed on by the hvc callbacks to us to
	 * differentiate between the other console ports handled by
	 * this driver
	 */
	u32 vtermno;
};

/*
 * This is a per-device struct that stores data common to all the
 * ports for that device (vdev->priv).
 */
struct ports_device {
	/* Array of per-port IO virtqueues */
	struct virtqueue **in_vqs, **out_vqs;

	struct virtio_device *vdev;
};

struct port_buffer {
	char *buf;

	/* size of the buffer in *buf above */
	size_t size;

	/* used length of the buffer */
	size_t len;
	/* offset in the buf from which to consume data */
	size_t offset;
};

/* This struct holds the per-port data */
struct port {
	/* Pointer to the parent virtio_console device */
	struct ports_device *portdev;

	/* The current buffer from which data has to be fed to readers */
	struct port_buffer *inbuf;

	/*
	 * To protect the operations on the in_vq associated with this
	 * port.  Has to be a spinlock because it can be called from
	 * interrupt context (get_char()).
	 */
	spinlock_t inbuf_lock;

	/* The IO vqs for this port */
	struct virtqueue *in_vq, *out_vq;

	/*
	 * The entries in this struct will be valid if this port is
	 * hooked up to an hvc console
	 */
	struct console cons;
};

/* This is the very early arch-specified put chars function. */
static int (*early_put_chars)(u32, const char *, int);

static struct port *find_port_by_vtermno(u32 vtermno)
{
	struct port *port;
	struct console *cons;
	unsigned long flags;

	spin_lock_irqsave(&pdrvdata_lock, flags);
	list_for_each_entry(cons, &pdrvdata.consoles, list) {
		if (cons->vtermno == vtermno) {
			port = container_of(cons, struct port, cons);
			goto out;
		}
	}
	port = NULL;
out:
	spin_unlock_irqrestore(&pdrvdata_lock, flags);
	return port;
}

static struct port *find_port_by_vq(struct ports_device *portdev,
				    struct virtqueue *vq)
{
	struct port *port;
	struct console *cons;
	unsigned long flags;

	spin_lock_irqsave(&pdrvdata_lock, flags);
	list_for_each_entry(cons, &pdrvdata.consoles, list) {
		port = container_of(cons, struct port, cons);
		if (port->in_vq == vq || port->out_vq == vq)
			goto out;
	}
	port = NULL;
out:
	spin_unlock_irqrestore(&pdrvdata_lock, flags);
	return port;
}

static void free_buf(struct port_buffer *buf)
{
	kfree(buf->buf);
	kfree(buf);
}

static struct port_buffer *alloc_buf(size_t buf_size)
{
	struct port_buffer *buf;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		goto fail;
	buf->buf = kzalloc(buf_size, GFP_KERNEL);
	if (!buf->buf)
		goto free_buf;
	buf->len = 0;
	buf->offset = 0;
	buf->size = buf_size;
	return buf;

free_buf:
	kfree(buf);
fail:
	return NULL;
}

/* Callers should take appropriate locks */
static void *get_inbuf(struct port *port)
{
	struct port_buffer *buf;
	struct virtqueue *vq;
	unsigned int len;

	vq = port->in_vq;
	buf = vq->vq_ops->get_buf(vq, &len);
	if (buf) {
		buf->len = len;
		buf->offset = 0;
	}
	return buf;
}

/*
 * Create a scatter-gather list representing our input buffer and put
 * it in the queue.
 *
 * Callers should take appropriate locks.
 */
static int add_inbuf(struct virtqueue *vq, struct port_buffer *buf)
{
	struct scatterlist sg[1];
	int ret;

	sg_init_one(sg, buf->buf, buf->size);

	ret = vq->vq_ops->add_buf(vq, sg, 0, 1, buf);
	vq->vq_ops->kick(vq);
	return ret;
}

static bool port_has_data(struct port *port)
{
	unsigned long flags;
	bool ret;

	ret = false;
	spin_lock_irqsave(&port->inbuf_lock, flags);
	if (port->inbuf)
		ret = true;
	spin_unlock_irqrestore(&port->inbuf_lock, flags);

	return ret;
}

static ssize_t send_buf(struct port *port, void *in_buf, size_t in_count)
{
	struct scatterlist sg[1];
	struct virtqueue *out_vq;
	ssize_t ret;
	unsigned int len;

	out_vq = port->out_vq;

	sg_init_one(sg, in_buf, in_count);
	ret = out_vq->vq_ops->add_buf(out_vq, sg, 1, 0, in_buf);

	/* Tell Host to go! */
	out_vq->vq_ops->kick(out_vq);

	if (ret < 0) {
		len = 0;
		goto fail;
	}

	/*
	 * Wait till the host acknowledges it pushed out the data we
	 * sent. Also ensure we return to userspace the number of
	 * bytes that were successfully consumed by the host.
	 */
	while (!out_vq->vq_ops->get_buf(out_vq, &len))
		cpu_relax();
fail:
	/* We're expected to return the amount of data we wrote */
	return len;
}

/*
 * Give out the data that's requested from the buffer that we have
 * queued up.
 */
static ssize_t fill_readbuf(struct port *port, char *out_buf, size_t out_count)
{
	struct port_buffer *buf;
	unsigned long flags;

	if (!out_count || !port_has_data(port))
		return 0;

	buf = port->inbuf;
	if (out_count > buf->len - buf->offset)
		out_count = buf->len - buf->offset;

	memcpy(out_buf, buf->buf + buf->offset, out_count);

	/* Return the number of bytes actually copied */
	buf->offset += out_count;

	if (buf->offset == buf->len) {
		/*
		 * We're done using all the data in this buffer.
		 * Re-queue so that the Host can send us more data.
		 */
		spin_lock_irqsave(&port->inbuf_lock, flags);
		port->inbuf = NULL;

		if (add_inbuf(port->in_vq, buf) < 0)
			dev_warn(&port->portdev->vdev->dev, "failed add_buf\n");

		spin_unlock_irqrestore(&port->inbuf_lock, flags);
	}
	return out_count;
}

/*
 * The put_chars() callback is pretty straightforward.
 *
 * We turn the characters into a scatter-gather list, add it to the
 * output queue and then kick the Host.  Then we sit here waiting for
 * it to finish: inefficient in theory, but in practice
 * implementations will do it immediately (lguest's Launcher does).
 */
static int put_chars(u32 vtermno, const char *buf, int count)
{
	struct port *port;

	port = find_port_by_vtermno(vtermno);
	if (!port)
		return 0;

	if (unlikely(early_put_chars))
		return early_put_chars(vtermno, buf, count);

	return send_buf(port, (void *)buf, count);
}

/*
 * get_chars() is the callback from the hvc_console infrastructure
 * when an interrupt is received.
 *
 * We call out to fill_readbuf that gets us the required data from the
 * buffers that are queued up.
 */
static int get_chars(u32 vtermno, char *buf, int count)
{
	struct port *port;

	port = find_port_by_vtermno(vtermno);
	if (!port)
		return 0;

	/* If we don't have an input queue yet, we can't get input. */
	BUG_ON(!port->in_vq);

	return fill_readbuf(port, buf, count);
}

static void resize_console(struct port *port)
{
	struct virtio_device *vdev;
	struct winsize ws;

	vdev = port->portdev->vdev;
	if (virtio_has_feature(vdev, VIRTIO_CONSOLE_F_SIZE)) {
		vdev->config->get(vdev,
				  offsetof(struct virtio_console_config, cols),
				  &ws.ws_col, sizeof(u16));
		vdev->config->get(vdev,
				  offsetof(struct virtio_console_config, rows),
				  &ws.ws_row, sizeof(u16));
		hvc_resize(port->cons.hvc, ws);
	}
}

static void virtcons_apply_config(struct virtio_device *vdev)
{
	resize_console(find_port_by_vtermno(0));
}

/* We set the configuration at this point, since we now have a tty */
static int notifier_add_vio(struct hvc_struct *hp, int data)
{
	struct port *port;

	port = find_port_by_vtermno(hp->vtermno);
	if (!port)
		return -EINVAL;

	hp->irq_requested = 1;
	resize_console(port);

	return 0;
}

static void notifier_del_vio(struct hvc_struct *hp, int data)
{
	hp->irq_requested = 0;
}

static void hvc_handle_input(struct virtqueue *vq)
{
	struct port *port;
	unsigned long flags;

	port = find_port_by_vq(vq->vdev->priv, vq);
	if (!port)
		return;

	spin_lock_irqsave(&port->inbuf_lock, flags);
	port->inbuf = get_inbuf(port);
	spin_unlock_irqrestore(&port->inbuf_lock, flags);

	if (hvc_poll(port->cons.hvc))
		hvc_kick();
}

/* The operations for the console. */
static const struct hv_ops hv_ops = {
	.get_chars = get_chars,
	.put_chars = put_chars,
	.notifier_add = notifier_add_vio,
	.notifier_del = notifier_del_vio,
	.notifier_hangup = notifier_del_vio,
};

/*
 * Console drivers are initialized very early so boot messages can go
 * out, so we do things slightly differently from the generic virtio
 * initialization of the net and block drivers.
 *
 * At this stage, the console is output-only.  It's too early to set
 * up a virtqueue, so we let the drivers do some boutique early-output
 * thing.
 */
int __init virtio_cons_early_init(int (*put_chars)(u32, const char *, int))
{
	early_put_chars = put_chars;
	return hvc_instantiate(0, 0, &hv_ops);
}

int __devinit init_port_console(struct port *port)
{
	int ret;

	/*
	 * The Host's telling us this port is a console port.  Hook it
	 * up with an hvc console.
	 *
	 * To set up and manage our virtual console, we call
	 * hvc_alloc().
	 *
	 * The first argument of hvc_alloc() is the virtual console
	 * number.  The second argument is the parameter for the
	 * notification mechanism (like irq number).  We currently
	 * leave this as zero, virtqueues have implicit notifications.
	 *
	 * The third argument is a "struct hv_ops" containing the
	 * put_chars() get_chars(), notifier_add() and notifier_del()
	 * pointers.  The final argument is the output buffer size: we
	 * can do any size, so we put PAGE_SIZE here.
	 */
	port->cons.vtermno = pdrvdata.next_vtermno;

	port->cons.hvc = hvc_alloc(port->cons.vtermno, 0, &hv_ops, PAGE_SIZE);
	if (IS_ERR(port->cons.hvc)) {
		ret = PTR_ERR(port->cons.hvc);
		port->cons.hvc = NULL;
		return ret;
	}
	spin_lock_irq(&pdrvdata_lock);
	pdrvdata.next_vtermno++;
	list_add_tail(&port->cons.list, &pdrvdata.consoles);
	spin_unlock_irq(&pdrvdata_lock);

	return 0;
}

static int __devinit add_port(struct ports_device *portdev)
{
	struct port *port;
	struct port_buffer *inbuf;
	int err;

	port = kmalloc(sizeof(*port), GFP_KERNEL);
	if (!port) {
		err = -ENOMEM;
		goto fail;
	}

	port->portdev = portdev;

	port->inbuf = NULL;

	port->in_vq = portdev->in_vqs[0];
	port->out_vq = portdev->out_vqs[0];

	spin_lock_init(&port->inbuf_lock);

	inbuf = alloc_buf(PAGE_SIZE);
	if (!inbuf) {
		err = -ENOMEM;
		goto free_port;
	}

	/* Register the input buffer the first time. */
	add_inbuf(port->in_vq, inbuf);

	err = init_port_console(port);
	if (err)
		goto free_inbuf;

	return 0;

free_inbuf:
	free_buf(inbuf);
free_port:
	kfree(port);
fail:
	return err;
}

static int init_vqs(struct ports_device *portdev)
{
	vq_callback_t **io_callbacks;
	char **io_names;
	struct virtqueue **vqs;
	u32 nr_ports, nr_queues;
	int err;

	/* We currently only have one port and two queues for that port */
	nr_ports = 1;
	nr_queues = 2;

	vqs = kmalloc(nr_queues * sizeof(struct virtqueue *), GFP_KERNEL);
	if (!vqs) {
		err = -ENOMEM;
		goto fail;
	}
	io_callbacks = kmalloc(nr_queues * sizeof(vq_callback_t *), GFP_KERNEL);
	if (!io_callbacks) {
		err = -ENOMEM;
		goto free_vqs;
	}
	io_names = kmalloc(nr_queues * sizeof(char *), GFP_KERNEL);
	if (!io_names) {
		err = -ENOMEM;
		goto free_callbacks;
	}
	portdev->in_vqs = kmalloc(nr_ports * sizeof(struct virtqueue *),
				  GFP_KERNEL);
	if (!portdev->in_vqs) {
		err = -ENOMEM;
		goto free_names;
	}
	portdev->out_vqs = kmalloc(nr_ports * sizeof(struct virtqueue *),
				   GFP_KERNEL);
	if (!portdev->out_vqs) {
		err = -ENOMEM;
		goto free_invqs;
	}

	io_callbacks[0] = hvc_handle_input;
	io_callbacks[1] = NULL;
	io_names[0] = "input";
	io_names[1] = "output";

	/* Find the queues. */
	err = portdev->vdev->config->find_vqs(portdev->vdev, nr_queues, vqs,
					      io_callbacks,
					      (const char **)io_names);
	if (err)
		goto free_outvqs;

	portdev->in_vqs[0] = vqs[0];
	portdev->out_vqs[0] = vqs[1];

	kfree(io_callbacks);
	kfree(io_names);
	kfree(vqs);

	return 0;

free_names:
	kfree(io_names);
free_callbacks:
	kfree(io_callbacks);
free_outvqs:
	kfree(portdev->out_vqs);
free_invqs:
	kfree(portdev->in_vqs);
free_vqs:
	kfree(vqs);
fail:
	return err;
}

/*
 * Once we're further in boot, we get probed like any other virtio
 * device.
 */
static int __devinit virtcons_probe(struct virtio_device *vdev)
{
	struct ports_device *portdev;
	int err;

	portdev = kmalloc(sizeof(*portdev), GFP_KERNEL);
	if (!portdev) {
		err = -ENOMEM;
		goto fail;
	}

	/* Attach this portdev to this virtio_device, and vice-versa. */
	portdev->vdev = vdev;
	vdev->priv = portdev;

	err = init_vqs(portdev);
	if (err < 0) {
		dev_err(&vdev->dev, "Error %d initializing vqs\n", err);
		goto free;
	}

	/* We only have one port. */
	err = add_port(portdev);
	if (err)
		goto free_vqs;

	/* Start using the new console output. */
	early_put_chars = NULL;
	return 0;

free_vqs:
	vdev->config->del_vqs(vdev);
	kfree(portdev->in_vqs);
	kfree(portdev->out_vqs);
free:
	kfree(portdev);
fail:
	return err;
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_CONSOLE, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static unsigned int features[] = {
	VIRTIO_CONSOLE_F_SIZE,
};

static struct virtio_driver virtio_console = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.driver.name =	KBUILD_MODNAME,
	.driver.owner =	THIS_MODULE,
	.id_table =	id_table,
	.probe =	virtcons_probe,
	.config_changed = virtcons_apply_config,
};

static int __init init(void)
{
	INIT_LIST_HEAD(&pdrvdata.consoles);

	return register_virtio_driver(&virtio_console);
}
module_init(init);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio console driver");
MODULE_LICENSE("GPL");
