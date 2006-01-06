/*
 * Apple USB Touchpad (for post-February 2005 PowerBooks) driver
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2005      Johannes Berg (johannes@sipsolutions.net)
 * Copyright (C) 2005      Stelian Pop (stelian@popies.net)
 * Copyright (C) 2005      Frank Arnold (frank@scirocco-5v-turbo.de)
 * Copyright (C) 2005      Peter Osterlund (petero2@telia.com)
 *
 * Thanks to Alex Harper <basilisk@foobox.net> for his inputs.
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/input.h>
#include <linux/usb_input.h>

/* Apple has powerbooks which have the keyboard with different Product IDs */
#define APPLE_VENDOR_ID		0x05AC

#define ATP_DEVICE(prod)					\
	.match_flags = USB_DEVICE_ID_MATCH_DEVICE |		\
		       USB_DEVICE_ID_MATCH_INT_CLASS |		\
		       USB_DEVICE_ID_MATCH_INT_PROTOCOL,	\
	.idVendor = APPLE_VENDOR_ID,				\
	.idProduct = (prod),					\
	.bInterfaceClass = 0x03,				\
	.bInterfaceProtocol = 0x02

/* table of devices that work with this driver */
static struct usb_device_id atp_table [] = {
	{ ATP_DEVICE(0x020E) },
	{ ATP_DEVICE(0x020F) },
	{ ATP_DEVICE(0x030A) },
	{ ATP_DEVICE(0x030B) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE (usb, atp_table);

/* size of a USB urb transfer */
#define ATP_DATASIZE	81

/*
 * number of sensors. Note that only 16 instead of 26 X (horizontal)
 * sensors exist on 12" and 15" PowerBooks. All models have 16 Y
 * (vertical) sensors.
 */
#define ATP_XSENSORS	26
#define ATP_YSENSORS	16

/* amount of fuzz this touchpad generates */
#define ATP_FUZZ	16

/* maximum pressure this driver will report */
#define ATP_PRESSURE	300
/*
 * multiplication factor for the X and Y coordinates.
 * We try to keep the touchpad aspect ratio while still doing only simple
 * arithmetics.
 * The factors below give coordinates like:
 *	0 <= x <  960 on 12" and 15" Powerbooks
 *	0 <= x < 1600 on 17" Powerbooks
 *	0 <= y <  646
 */
#define ATP_XFACT	64
#define ATP_YFACT	43

/*
 * Threshold for the touchpad sensors. Any change less than ATP_THRESHOLD is
 * ignored.
 */
#define ATP_THRESHOLD	 5

/* Structure to hold all of our device specific stuff */
struct atp {
	char			phys[64];
	struct usb_device *	udev;		/* usb device */
	struct urb *		urb;		/* usb request block */
	signed char *		data;		/* transferred data */
	int			open;		/* non-zero if opened */
	struct input_dev	*input;		/* input dev */
	int			valid;		/* are the sensors valid ? */
	int			x_old;		/* last reported x/y, */
	int			y_old;		/* used for smoothing */
						/* current value of the sensors */
	signed char		xy_cur[ATP_XSENSORS + ATP_YSENSORS];
						/* last value of the sensors */
	signed char		xy_old[ATP_XSENSORS + ATP_YSENSORS];
						/* accumulated sensors */
	int			xy_acc[ATP_XSENSORS + ATP_YSENSORS];
};

#define dbg_dump(msg, tab) \
	if (debug > 1) {						\
		int i;							\
		printk("appletouch: %s %lld", msg, (long long)jiffies); \
		for (i = 0; i < ATP_XSENSORS + ATP_YSENSORS; i++)	\
			printk(" %02x", tab[i]);			\
		printk("\n");						\
	}

#define dprintk(format, a...)						\
	do {								\
		if (debug) printk(format, ##a);				\
	} while (0)

MODULE_AUTHOR("Johannes Berg, Stelian Pop, Frank Arnold");
MODULE_DESCRIPTION("Apple PowerBooks USB touchpad driver");
MODULE_LICENSE("GPL");

static int debug = 1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Activate debugging output");

static int atp_calculate_abs(int *xy_sensors, int nb_sensors, int fact,
			     int *z, int *fingers)
{
	int i;
	/* values to calculate mean */
	int pcum = 0, psum = 0;

	*fingers = 0;

	for (i = 0; i < nb_sensors; i++) {
		if (xy_sensors[i] < ATP_THRESHOLD)
			continue;
		if ((i - 1 < 0) || (xy_sensors[i - 1] < ATP_THRESHOLD))
			(*fingers)++;
		pcum += xy_sensors[i] * i;
		psum += xy_sensors[i];
	}

	if (psum > 0) {
		*z = psum;
		return pcum * fact / psum;
	}

	return 0;
}

static inline void atp_report_fingers(struct input_dev *input, int fingers)
{
	input_report_key(input, BTN_TOOL_FINGER, fingers == 1);
	input_report_key(input, BTN_TOOL_DOUBLETAP, fingers == 2);
	input_report_key(input, BTN_TOOL_TRIPLETAP, fingers > 2);
}

static void atp_complete(struct urb* urb, struct pt_regs* regs)
{
	int x, y, x_z, y_z, x_f, y_f;
	int retval, i;
	struct atp *dev = urb->context;

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* This urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d",
		    __FUNCTION__, urb->status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d",
		    __FUNCTION__, urb->status);
		goto exit;
	}

	/* drop incomplete datasets */
	if (dev->urb->actual_length != ATP_DATASIZE) {
		dprintk("appletouch: incomplete data package.\n");
		goto exit;
	}

	/* reorder the sensors values */
	for (i = 0; i < 8; i++) {
		/* X values */
		dev->xy_cur[i     ] = dev->data[5 * i +  2];
		dev->xy_cur[i +  8] = dev->data[5 * i +  4];
		dev->xy_cur[i + 16] = dev->data[5 * i + 42];
		if (i < 2)
			dev->xy_cur[i + 24] = dev->data[5 * i + 44];

		/* Y values */
		dev->xy_cur[i + 26] = dev->data[5 * i +  1];
		dev->xy_cur[i + 34] = dev->data[5 * i +  3];
	}

	dbg_dump("sample", dev->xy_cur);

	if (!dev->valid) {
		/* first sample */
		dev->valid = 1;
		dev->x_old = dev->y_old = -1;
		memcpy(dev->xy_old, dev->xy_cur, sizeof(dev->xy_old));

		/* 17" Powerbooks have 10 extra X sensors */
		for (i = 16; i < ATP_XSENSORS; i++)
			if (dev->xy_cur[i]) {
				printk("appletouch: 17\" model detected.\n");
				input_set_abs_params(dev->input, ABS_X, 0,
						     (ATP_XSENSORS - 1) *
						     ATP_XFACT - 1,
						     ATP_FUZZ, 0);
				break;
			}

		goto exit;
	}

	for (i = 0; i < ATP_XSENSORS + ATP_YSENSORS; i++) {
		/* accumulate the change */
		signed char change = dev->xy_old[i] - dev->xy_cur[i];
		dev->xy_acc[i] -= change;

		/* prevent down drifting */
		if (dev->xy_acc[i] < 0)
			dev->xy_acc[i] = 0;
	}

	memcpy(dev->xy_old, dev->xy_cur, sizeof(dev->xy_old));

	dbg_dump("accumulator", dev->xy_acc);

	x = atp_calculate_abs(dev->xy_acc, ATP_XSENSORS,
			      ATP_XFACT, &x_z, &x_f);
	y = atp_calculate_abs(dev->xy_acc + ATP_XSENSORS, ATP_YSENSORS,
			      ATP_YFACT, &y_z, &y_f);

	if (x && y) {
		if (dev->x_old != -1) {
			x = (dev->x_old * 3 + x) >> 2;
			y = (dev->y_old * 3 + y) >> 2;
			dev->x_old = x;
			dev->y_old = y;

			if (debug > 1)
				printk("appletouch: X: %3d Y: %3d "
				       "Xz: %3d Yz: %3d\n",
				       x, y, x_z, y_z);

			input_report_key(dev->input, BTN_TOUCH, 1);
			input_report_abs(dev->input, ABS_X, x);
			input_report_abs(dev->input, ABS_Y, y);
			input_report_abs(dev->input, ABS_PRESSURE,
					 min(ATP_PRESSURE, x_z + y_z));
			atp_report_fingers(dev->input, max(x_f, y_f));
		}
		dev->x_old = x;
		dev->y_old = y;
	}
	else if (!x && !y) {

		dev->x_old = dev->y_old = -1;
		input_report_key(dev->input, BTN_TOUCH, 0);
		input_report_abs(dev->input, ABS_PRESSURE, 0);
		atp_report_fingers(dev->input, 0);

		/* reset the accumulator on release */
		memset(dev->xy_acc, 0, sizeof(dev->xy_acc));
	}

	input_report_key(dev->input, BTN_LEFT, !!dev->data[80]);

	input_sync(dev->input);

exit:
	retval = usb_submit_urb(dev->urb, GFP_ATOMIC);
	if (retval) {
		err("%s - usb_submit_urb failed with result %d",
		    __FUNCTION__, retval);
	}
}

static int atp_open(struct input_dev *input)
{
	struct atp *dev = input->private;

	if (usb_submit_urb(dev->urb, GFP_ATOMIC))
		return -EIO;

	dev->open = 1;
	return 0;
}

static void atp_close(struct input_dev *input)
{
	struct atp *dev = input->private;

	usb_kill_urb(dev->urb);
	dev->open = 0;
}

static int atp_probe(struct usb_interface *iface, const struct usb_device_id *id)
{
	struct atp *dev;
	struct input_dev *input_dev;
	struct usb_device *udev = interface_to_usbdev(iface);
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	int int_in_endpointAddr = 0;
	int i, retval = -ENOMEM;


	/* set up the endpoint information */
	/* use only the first interrupt-in endpoint */
	iface_desc = iface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		endpoint = &iface_desc->endpoint[i].desc;
		if (!int_in_endpointAddr &&
		    (endpoint->bEndpointAddress & USB_DIR_IN) &&
		    ((endpoint->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK)
					== USB_ENDPOINT_XFER_INT)) {
			/* we found an interrupt in endpoint */
			int_in_endpointAddr = endpoint->bEndpointAddress;
			break;
		}
	}
	if (!int_in_endpointAddr) {
		err("Could not find int-in endpoint");
		return -EIO;
	}

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(struct atp), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!dev || !input_dev) {
		err("Out of memory");
		goto err_free_devs;
	}

	dev->udev = udev;
	dev->input = input_dev;

	dev->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->urb) {
		retval = -ENOMEM;
		goto err_free_devs;
	}

	dev->data = usb_buffer_alloc(dev->udev, ATP_DATASIZE, GFP_KERNEL,
				     &dev->urb->transfer_dma);
	if (!dev->data) {
		retval = -ENOMEM;
		goto err_free_urb;
	}

	usb_fill_int_urb(dev->urb, udev,
			 usb_rcvintpipe(udev, int_in_endpointAddr),
			 dev->data, ATP_DATASIZE, atp_complete, dev, 1);

	usb_make_path(udev, dev->phys, sizeof(dev->phys));
	strlcat(dev->phys, "/input0", sizeof(dev->phys));

	input_dev->name = "appletouch";
	input_dev->phys = dev->phys;
	usb_to_input_id(dev->udev, &input_dev->id);
	input_dev->cdev.dev = &iface->dev;

	input_dev->private = dev;
	input_dev->open = atp_open;
	input_dev->close = atp_close;

	set_bit(EV_ABS, input_dev->evbit);

	/*
	 * 12" and 15" Powerbooks only have 16 x sensors,
	 * 17" models are detected later.
	 */
	input_set_abs_params(input_dev, ABS_X, 0,
			     (16 - 1) * ATP_XFACT - 1, ATP_FUZZ, 0);
	input_set_abs_params(input_dev, ABS_Y, 0,
			     (ATP_YSENSORS - 1) * ATP_YFACT - 1, ATP_FUZZ, 0);
	input_set_abs_params(input_dev, ABS_PRESSURE, 0, ATP_PRESSURE, 0, 0);

	set_bit(EV_KEY, input_dev->evbit);
	set_bit(BTN_TOUCH, input_dev->keybit);
	set_bit(BTN_TOOL_FINGER, input_dev->keybit);
	set_bit(BTN_TOOL_DOUBLETAP, input_dev->keybit);
	set_bit(BTN_TOOL_TRIPLETAP, input_dev->keybit);
	set_bit(BTN_LEFT, input_dev->keybit);

	input_register_device(dev->input);

	/* save our data pointer in this interface device */
	usb_set_intfdata(iface, dev);

	return 0;

 err_free_urb:
	usb_free_urb(dev->urb);
 err_free_devs:
	usb_set_intfdata(iface, NULL);
	kfree(dev);
	input_free_device(input_dev);
	return retval;
}

static void atp_disconnect(struct usb_interface *iface)
{
	struct atp *dev = usb_get_intfdata(iface);

	usb_set_intfdata(iface, NULL);
	if (dev) {
		usb_kill_urb(dev->urb);
		input_unregister_device(dev->input);
		usb_free_urb(dev->urb);
		usb_buffer_free(dev->udev, ATP_DATASIZE,
				dev->data, dev->urb->transfer_dma);
		kfree(dev);
	}
	printk(KERN_INFO "input: appletouch disconnected\n");
}

static int atp_suspend(struct usb_interface *iface, pm_message_t message)
{
	struct atp *dev = usb_get_intfdata(iface);
	usb_kill_urb(dev->urb);
	dev->valid = 0;
	return 0;
}

static int atp_resume(struct usb_interface *iface)
{
	struct atp *dev = usb_get_intfdata(iface);
	if (dev->open && usb_submit_urb(dev->urb, GFP_ATOMIC))
		return -EIO;

	return 0;
}

static struct usb_driver atp_driver = {
	.name		= "appletouch",
	.probe		= atp_probe,
	.disconnect	= atp_disconnect,
	.suspend	= atp_suspend,
	.resume		= atp_resume,
	.id_table	= atp_table,
};

static int __init atp_init(void)
{
	return usb_register(&atp_driver);
}

static void __exit atp_exit(void)
{
	usb_deregister(&atp_driver);
}

module_init(atp_init);
module_exit(atp_exit);
