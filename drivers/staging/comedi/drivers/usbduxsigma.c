/*
   comedi/drivers/usbdux.c
   Copyright (C) 2011 Bernd Porr, Bernd.Porr@f2s.com

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
 */
/*
Driver: usbduxsigma
Description: University of Stirling USB DAQ & INCITE Technology Limited
Devices: [ITL] USB-DUX (usbduxsigma.o)
Author: Bernd Porr <BerndPorr@f2s.com>
Updated: 8 Nov 2011
Status: testing
*/
/*
 * I must give credit here to Chris Baugher who
 * wrote the driver for AT-MIO-16d. I used some parts of this
 * driver. I also must give credits to David Brownell
 * who supported me with the USB development.
 *
 * Note: the raw data from the A/D converter is 24 bit big endian
 * anything else is little endian to/from the dux board
 *
 *
 * Revision history:
 *   0.1: initial version
 *   0.2: all basic functions implemented, digital I/O only for one port
 *   0.3: proper vendor ID and driver name
 *   0.4: fixed D/A voltage range
 *   0.5: various bug fixes, health check at startup
 *   0.6: corrected wrong input range
 */

/* generates loads of debug info */
/* #define NOISY_DUX_DEBUGBUG */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/fcntl.h>
#include <linux/compiler.h>

#include "comedi_fc.h"
#include "../comedidev.h"

/* timeout for the USB-transfer in ms*/
#define BULK_TIMEOUT 1000

/* constants for "firmware" upload and download */
#define FIRMWARE "usbduxsigma_firmware.bin"
#define USBDUXSUB_FIRMWARE 0xA0
#define VENDOR_DIR_IN  0xC0
#define VENDOR_DIR_OUT 0x40

/* internal addresses of the 8051 processor */
#define USBDUXSUB_CPUCS 0xE600

/*
 * the minor device number, major is 180 only for debugging purposes and to
 * upload special firmware (programming the eeprom etc) which is not
 * compatible with the comedi framwork
 */
#define USBDUXSUB_MINOR 32

/* max lenghth of the transfer-buffer for software upload */
#define TB_LEN 0x2000

/* Input endpoint number: ISO/IRQ */
#define ISOINEP           6

/* Output endpoint number: ISO/IRQ */
#define ISOOUTEP          2

/* This EP sends DUX commands to USBDUX */
#define COMMAND_OUT_EP     1

/* This EP receives the DUX commands from USBDUX */
#define COMMAND_IN_EP        8

/* Output endpoint for PWM */
#define PWM_EP         4

/* 300Hz max frequ under PWM */
#define MIN_PWM_PERIOD  ((long)(1E9/300))

/* Default PWM frequency */
#define PWM_DEFAULT_PERIOD ((long)(1E9/100))

/* Number of channels (16 AD and offset)*/
#define NUMCHANNELS 16

/* Size of one A/D value */
#define SIZEADIN          ((sizeof(int32_t)))

/*
 * Size of the async input-buffer IN BYTES, the DIO state is transmitted
 * as the first byte.
 */
#define SIZEINBUF         (((NUMCHANNELS+1)*SIZEADIN))

/* 16 bytes. */
#define SIZEINSNBUF       16

/* Number of DA channels */
#define NUMOUTCHANNELS    8

/* size of one value for the D/A converter: channel and value */
#define SIZEDAOUT          ((sizeof(uint8_t)+sizeof(int16_t)))

/*
 * Size of the output-buffer in bytes
 * Actually only the first 4 triplets are used but for the
 * high speed mode we need to pad it to 8 (microframes).
 */
#define SIZEOUTBUF         ((8*SIZEDAOUT))

/*
 * Size of the buffer for the dux commands: just now max size is determined
 * by the analogue out + command byte + panic bytes...
 */
#define SIZEOFDUXBUFFER    ((8*SIZEDAOUT+2))

/* Number of in-URBs which receive the data: min=2 */
#define NUMOFINBUFFERSFULL     5

/* Number of out-URBs which send the data: min=2 */
#define NUMOFOUTBUFFERSFULL    5

/* Number of in-URBs which receive the data: min=5 */
/* must have more buffers due to buggy USB ctr */
#define NUMOFINBUFFERSHIGH     10

/* Number of out-URBs which send the data: min=5 */
/* must have more buffers due to buggy USB ctr */
#define NUMOFOUTBUFFERSHIGH    10

/* Total number of usbdux devices */
#define NUMUSBDUX             16

/* Analogue in subdevice */
#define SUBDEV_AD             0

/* Analogue out subdevice */
#define SUBDEV_DA             1

/* Digital I/O */
#define SUBDEV_DIO            2

/* timer aka pwm output */
#define SUBDEV_PWM            3

/* number of retries to get the right dux command */
#define RETRIES 10

/**************************************************/
/* comedi constants */
static const struct comedi_lrange range_usbdux_ai_range = { 1, {
								BIP_RANGE
								(2.65/2.0)
								}
};

/*
 * private structure of one subdevice
 */

/*
 * This is the structure which holds all the data of
 * this driver one sub device just now: A/D
 */
struct usbduxsub {
	/* attached? */
	int attached;
	/* is it associated with a subdevice? */
	int probed;
	/* pointer to the usb-device */
	struct usb_device *usbdev;
	/* actual number of in-buffers */
	int numOfInBuffers;
	/* actual number of out-buffers */
	int numOfOutBuffers;
	/* ISO-transfer handling: buffers */
	struct urb **urbIn;
	struct urb **urbOut;
	/* pwm-transfer handling */
	struct urb *urbPwm;
	/* PWM period */
	unsigned int pwmPeriod;
	/* PWM internal delay for the GPIF in the FX2 */
	uint8_t pwmDelay;
	/* size of the PWM buffer which holds the bit pattern */
	int sizePwmBuf;
	/* input buffer for the ISO-transfer */
	int32_t *inBuffer;
	/* input buffer for single insn */
	int8_t *insnBuffer;
	/* output buffer for single DA outputs */
	int16_t *outBuffer;
	/* interface number */
	int ifnum;
	/* interface structure in 2.6 */
	struct usb_interface *interface;
	/* comedi device for the interrupt context */
	struct comedi_device *comedidev;
	/* is it USB_SPEED_HIGH or not? */
	short int high_speed;
	/* asynchronous command is running */
	short int ai_cmd_running;
	short int ao_cmd_running;
	/* pwm is running */
	short int pwm_cmd_running;
	/* continuous acquisition */
	short int ai_continuous;
	short int ao_continuous;
	/* number of samples to acquire */
	int ai_sample_count;
	int ao_sample_count;
	/* time between samples in units of the timer */
	unsigned int ai_timer;
	unsigned int ao_timer;
	/* counter between acquisitions */
	unsigned int ai_counter;
	unsigned int ao_counter;
	/* interval in frames/uframes */
	unsigned int ai_interval;
	/* D/A commands */
	uint8_t *dac_commands;
	/* commands */
	uint8_t *dux_commands;
	struct semaphore sem;
};

/*
 * The pointer to the private usb-data of the driver is also the private data
 * for the comedi-device.  This has to be global as the usb subsystem needs
 * global variables. The other reason is that this structure must be there
 * _before_ any comedi command is issued. The usb subsystem must be initialised
 * before comedi can access it.
 */
static struct usbduxsub usbduxsub[NUMUSBDUX];

static DEFINE_SEMAPHORE(start_stop_sem);

static void usbdux_ai_stop(struct usbduxsub *devpriv, int do_unlink)
{
	if (do_unlink) {
		int i;

		for (i = 0; i < devpriv->numOfInBuffers; i++) {
			if (devpriv->urbIn[i])
				usb_kill_urb(devpriv->urbIn[i]);
		}
	}

	devpriv->ai_cmd_running = 0;
}

static int usbdux_ai_cancel(struct comedi_device *dev,
			    struct comedi_subdevice *s)
{
	struct usbduxsub *devpriv = dev->private;

	down(&devpriv->sem);
	/* unlink only if it is really running */
	usbdux_ai_stop(devpriv, devpriv->ai_cmd_running);
	up(&devpriv->sem);

	return 0;
}

static void usbduxsub_ai_IsocIrq(struct urb *urb)
{
	struct comedi_device *dev = urb->context;
	struct usbduxsub *devpriv = dev->private;
	struct comedi_subdevice *s = dev->read_subdev;
	unsigned int dio_state;
	int32_t val;
	int ret;
	int i;

	/* first we test if something unusual has just happened */
	switch (urb->status) {
	case 0:
		/* copy the result in the transfer buffer */
		memcpy(devpriv->inBuffer, urb->transfer_buffer, SIZEINBUF);
		break;
	case -EILSEQ:
		/*
		 * error in the ISOchronous data
		 * we don't copy the data into the transfer buffer
		 * and recycle the last data byte
		 */
		dev_dbg(dev->class_dev,"CRC error in ISO IN stream\n");

		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -ECONNABORTED:
		/* happens after an unlink command */
		if (devpriv->ai_cmd_running) {
			usbdux_ai_stop(devpriv, 0);	/* w/o unlink */
			/* we are still running a command, tell comedi */
			s->async->events |= (COMEDI_CB_EOA | COMEDI_CB_ERROR);
			comedi_event(dev, s);
		}
		return;

	default:
		/*
		 * a real error on the bus
		 * pass error to comedi if we are really running a command
		 */
		if (devpriv->ai_cmd_running) {
			dev_err(dev->class_dev,
				"%s: non-zero urb status (%d)\n",
				__func__, urb->status);
			usbdux_ai_stop(devpriv, 0);	/* w/o unlink */
			s->async->events |= (COMEDI_CB_EOA | COMEDI_CB_ERROR);
			comedi_event(dev, s);
		}
		return;
	}

	if (unlikely(!devpriv->ai_cmd_running))
		return;

	urb->dev = devpriv->usbdev;

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (unlikely(ret < 0)) {
		dev_err(dev->class_dev, "%s: urb resubmit failed (%d)\n",
			__func__, ret);
		if (ret == -EL2NSYNC)
			dev_err(dev->class_dev,
				"buggy USB host controller or bug in IRQ handler\n");
		usbdux_ai_stop(devpriv, 0);	/* w/o unlink */
		s->async->events |= (COMEDI_CB_EOA | COMEDI_CB_ERROR);
		comedi_event(dev, s);
		return;
	}

	/* get the state of the dio pins to allow external trigger */
	dio_state = be32_to_cpu(devpriv->inBuffer[0]);

	devpriv->ai_counter--;
	if (likely(devpriv->ai_counter > 0))
		return;

	/* timer zero, transfer measurements to comedi */
	devpriv->ai_counter = devpriv->ai_timer;

	if (!devpriv->ai_continuous) {
		/* not continuous, fixed number of samples */
		devpriv->ai_sample_count--;
		if (devpriv->ai_sample_count < 0) {
			usbdux_ai_stop(devpriv, 0);	/* w/o unlink */
			/* acquistion is over, tell comedi */
			s->async->events |= COMEDI_CB_EOA;
			comedi_event(dev, s);
			return;
		}
	}

	/* get the data from the USB bus and hand it over to comedi */
	for (i = 0; i < s->async->cmd.chanlist_len; i++) {
		/* transfer data, note first byte is the DIO state */
		val = be32_to_cpu(devpriv->inBuffer[i+1]);
		val &= 0x00ffffff;	/* strip status byte */
		val ^= 0x00800000;	/* convert to unsigned */

		ret = cfc_write_array_to_buffer(s, &val, sizeof(uint32_t));
		if (unlikely(ret == 0)) {
			/* buffer overflow */
			usbdux_ai_stop(devpriv, 0);	/* w/o unlink */
			return;
		}
	}
	/* tell comedi that data is there */
	s->async->events |= (COMEDI_CB_BLOCK | COMEDI_CB_EOS);
	comedi_event(dev, s);
}

static void usbdux_ao_stop(struct usbduxsub *devpriv, int do_unlink)
{
	if (do_unlink) {
		int i;

		for (i = 0; i < devpriv->numOfOutBuffers; i++) {
			if (devpriv->urbOut[i])
				usb_kill_urb(devpriv->urbOut[i]);
		}
	}

	devpriv->ao_cmd_running = 0;
}

static int usbdux_ao_cancel(struct comedi_device *dev,
			    struct comedi_subdevice *s)
{
	struct usbduxsub *devpriv = dev->private;

	down(&devpriv->sem);
	/* unlink only if it is really running */
	usbdux_ao_stop(devpriv, devpriv->ao_cmd_running);
	up(&devpriv->sem);

	return 0;
}

static void usbduxsub_ao_IsocIrq(struct urb *urb)
{
	struct comedi_device *dev = urb->context;
	struct usbduxsub *devpriv = dev->private;
	struct comedi_subdevice *s = dev->write_subdev;
	uint8_t *datap;
	int len;
	int ret;
	int i;

	switch (urb->status) {
	case 0:
		/* success */
		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -ECONNABORTED:
		/* happens after an unlink command */
		if (devpriv->ao_cmd_running) {
			usbdux_ao_stop(devpriv, 0);	/* w/o unlink */
			s->async->events |= COMEDI_CB_EOA;
			comedi_event(devpriv->comedidev, s);
		}
		return;

	default:
		/* a real error */
		if (devpriv->ao_cmd_running) {
			dev_err(dev->class_dev,
				"%s: non-zero urb status (%d)\n",
				__func__, urb->status);
			usbdux_ao_stop(devpriv, 0);	/* w/o unlink */
			s->async->events |= (COMEDI_CB_ERROR | COMEDI_CB_EOA);
			comedi_event(devpriv->comedidev, s);
		}
		return;
	}

	if (!devpriv->ao_cmd_running)
		return;

	devpriv->ao_counter--;
	if ((int)devpriv->ao_counter <= 0) {
		/* timer zero, transfer from comedi */
		devpriv->ao_counter = devpriv->ao_timer;

		if (!devpriv->ao_continuous) {
			/* not continuous, fixed number of samples */
			devpriv->ao_sample_count--;
			if (devpriv->ao_sample_count < 0) {
				usbdux_ao_stop(devpriv, 0);	/* w/o unlink */
				/* acquistion is over, tell comedi */
				s->async->events |= COMEDI_CB_EOA;
				comedi_event(devpriv->comedidev, s);
				return;
			}
		}

		/* transmit data to the USB bus */
		datap = urb->transfer_buffer;
		len = s->async->cmd.chanlist_len;
		*datap++ = len;
		for (i = 0; i < len; i++) {
			short val;

			if (i >= NUMOUTCHANNELS)
				break;

			ret = comedi_buf_get(s->async, &val);
			if (ret < 0) {
				dev_err(dev->class_dev, "buffer underflow\n");
				s->async->events |= (COMEDI_CB_EOA |
						     COMEDI_CB_OVERFLOW);
			}
			*datap++ = val;
			*datap++ = devpriv->dac_commands[i];

			s->async->events |= COMEDI_CB_BLOCK;
			comedi_event(dev, s);
		}
	}

	urb->transfer_buffer_length = SIZEOUTBUF;
	urb->dev = devpriv->usbdev;
	urb->status = 0;
	if (devpriv->high_speed)
		urb->interval = 8;	/* uframes */
	else
		urb->interval = 1;	/* frames */
	urb->number_of_packets = 1;
	urb->iso_frame_desc[0].offset = 0;
	urb->iso_frame_desc[0].length = SIZEOUTBUF;
	urb->iso_frame_desc[0].status = 0;
	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret < 0) {
		dev_err(dev->class_dev,
			"%s: urb resubmit failed (%d)\n",
			__func__, ret);
		if (ret == EL2NSYNC)
			dev_err(dev->class_dev,
				"buggy USB host controller or bug in IRQ handler\n");
		usbdux_ao_stop(devpriv, 0);	/* w/o unlink */
		s->async->events |= (COMEDI_CB_EOA | COMEDI_CB_ERROR);
		comedi_event(dev, s);
	}
}

/* the FX2LP has twice as much as the standard FX2 */
#define FIRMWARE_MAX_LEN 0x4000

static int usbduxsigma_firmware_upload(struct comedi_device *dev,
				       const u8 *data, size_t size,
				       unsigned long context)
{
	struct usbduxsub *usbduxsub = dev->private;
	struct usb_device *usb = usbduxsub->usbdev;
	uint8_t *buf;
	uint8_t *tmp;
	int ret;

	if (!data)
		return 0;

	if (size > FIRMWARE_MAX_LEN) {
		dev_err(&usbduxsub->interface->dev,
			"usbduxsigma firmware binary it too large for FX2.\n");
		return -ENOMEM;
	}

	/* we generate a local buffer for the firmware */
	buf = kmemdup(data, size, GFP_KERNEL);
	if (!buf) {
		dev_err(&usbduxsub->interface->dev,
			"comedi_: mem alloc for firmware failed\n");
		return -ENOMEM;
	}

	/* we need a malloc'ed buffer for usb_control_msg() */
	tmp = kmalloc(1, GFP_KERNEL);
	if (!tmp) {
		kfree(buf);
		return -ENOMEM;
	}

	/* stop the current firmware on the device */
	*tmp = 1;	/* 7f92 to one */
	ret = usb_control_msg(usb, usb_sndctrlpipe(usb, 0),
			      USBDUXSUB_FIRMWARE,
			      VENDOR_DIR_OUT,
			      USBDUXSUB_CPUCS, 0x0000,
			      tmp, 1,
			      BULK_TIMEOUT);
	if (ret < 0) {
		dev_err(&usbduxsub->interface->dev,
			"comedi_: can not stop firmware\n");
		goto done;
	}

	/* upload the new firmware to the device */
	ret = usb_control_msg(usb, usb_sndctrlpipe(usb, 0),
			      USBDUXSUB_FIRMWARE,
			      VENDOR_DIR_OUT,
			      0, 0x0000,
			      buf, size,
			      BULK_TIMEOUT);
	if (ret < 0) {
		dev_err(&usbduxsub->interface->dev,
			"comedi_: firmware upload failed\n");
		goto done;
	}

	/* start the new firmware on the device */
	*tmp = 0;	/* 7f92 to zero */
	ret = usb_control_msg(usb, usb_sndctrlpipe(usb, 0),
			      USBDUXSUB_FIRMWARE,
			      VENDOR_DIR_OUT,
			      USBDUXSUB_CPUCS, 0x0000,
			      tmp, 1,
			      BULK_TIMEOUT);
	if (ret < 0)
		dev_err(&usbduxsub->interface->dev,
			"comedi_: can not start firmware\n");

done:
	kfree(tmp);
	kfree(buf);
	return ret;
}

static int usbduxsigma_submit_urbs(struct comedi_device *dev,
				   struct urb **urbs, int num_urbs,
				   int input_urb)
{
	struct usbduxsub *devpriv = dev->private;
	struct urb *urb;
	int ret;
	int i;

	/* Submit all URBs and start the transfer on the bus */
	for (i = 0; i < num_urbs; i++) {
		urb = urbs[i];

		/* in case of a resubmission after an unlink... */
		if (input_urb)
			urb->interval = devpriv->ai_interval;
		urb->context = dev;
		urb->dev = devpriv->usbdev;
		urb->status = 0;
		urb->transfer_flags = URB_ISO_ASAP;

		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret)
			return ret;
	}
	return 0;
}

static int chanToInterval(int nChannels)
{
	if (nChannels <= 2)
		/* 4kHz */
		return 2;
	if (nChannels <= 8)
		/* 2kHz */
		return 4;
	/* 1kHz */
	return 8;
}

static int usbdux_ai_cmdtest(struct comedi_device *dev,
			     struct comedi_subdevice *s,
			     struct comedi_cmd *cmd)
{
	struct usbduxsub *this_usbduxsub = dev->private;
	int err = 0, i;
	unsigned int tmpTimer;

	/* Step 1 : check if triggers are trivially valid */

	err |= cfc_check_trigger_src(&cmd->start_src, TRIG_NOW | TRIG_INT);
	err |= cfc_check_trigger_src(&cmd->scan_begin_src, TRIG_TIMER);
	err |= cfc_check_trigger_src(&cmd->convert_src, TRIG_NOW);
	err |= cfc_check_trigger_src(&cmd->scan_end_src, TRIG_COUNT);
	err |= cfc_check_trigger_src(&cmd->stop_src, TRIG_COUNT | TRIG_NONE);

	if (err)
		return 1;

	/* Step 2a : make sure trigger sources are unique */

	err |= cfc_check_trigger_is_unique(cmd->start_src);
	err |= cfc_check_trigger_is_unique(cmd->stop_src);

	/* Step 2b : and mutually compatible */

	if (err)
		return 2;

	/* Step 3: check if arguments are trivially valid */

	err |= cfc_check_trigger_arg_is(&cmd->start_arg, 0);

	if (cmd->scan_begin_src == TRIG_FOLLOW)	/* internal trigger */
		err |= cfc_check_trigger_arg_is(&cmd->scan_begin_arg, 0);

	if (cmd->scan_begin_src == TRIG_TIMER) {
		if (this_usbduxsub->high_speed) {
			/*
			 * In high speed mode microframes are possible.
			 * However, during one microframe we can roughly
			 * sample two channels. Thus, the more channels
			 * are in the channel list the more time we need.
			 */
			i = chanToInterval(cmd->chanlist_len);
			err |= cfc_check_trigger_arg_min(&cmd->scan_begin_arg,
							 (1000000 / 8 * i));
			/* now calc the real sampling rate with all the
			 * rounding errors */
			tmpTimer =
			    ((unsigned int)(cmd->scan_begin_arg / 125000)) *
			    125000;
		} else {
			/* full speed */
			/* 1kHz scans every USB frame */
			err |= cfc_check_trigger_arg_min(&cmd->scan_begin_arg,
							 1000000);
			/*
			 * calc the real sampling rate with the rounding errors
			 */
			tmpTimer = ((unsigned int)(cmd->scan_begin_arg /
						   1000000)) * 1000000;
		}
		err |= cfc_check_trigger_arg_is(&cmd->scan_begin_arg,
						tmpTimer);
	}

	err |= cfc_check_trigger_arg_is(&cmd->scan_end_arg, cmd->chanlist_len);

	if (cmd->stop_src == TRIG_COUNT) {
		/* any count is allowed */
	} else {
		/* TRIG_NONE */
		err |= cfc_check_trigger_arg_is(&cmd->stop_arg, 0);
	}

	if (err)
		return 3;

	return 0;
}

/*
 * creates the ADC command for the MAX1271
 * range is the range value from comedi
 */
static void create_adc_command(unsigned int chan,
			       uint8_t *muxsg0,
			       uint8_t *muxsg1)
{
	if (chan < 8)
		(*muxsg0) = (*muxsg0) | (1 << chan);
	else if (chan < 16)
		(*muxsg1) = (*muxsg1) | (1 << (chan-8));
}


/* bulk transfers to usbdux */

#define SENDADCOMMANDS            0
#define SENDDACOMMANDS            1
#define SENDDIOCONFIGCOMMAND      2
#define SENDDIOBITSCOMMAND        3
#define SENDSINGLEAD              4
#define SENDPWMON                 7
#define SENDPWMOFF                8

static int send_dux_commands(struct usbduxsub *this_usbduxsub, int cmd_type)
{
	int result, nsent;

	this_usbduxsub->dux_commands[0] = cmd_type;
#ifdef NOISY_DUX_DEBUGBUG
	printk(KERN_DEBUG "comedi%d: usbdux: dux_commands: ",
	       this_usbduxsub->comedidev->minor);
	for (result = 0; result < SIZEOFDUXBUFFER; result++)
		printk(" %02x", this_usbduxsub->dux_commands[result]);
	printk("\n");
#endif
	result = usb_bulk_msg(this_usbduxsub->usbdev,
			      usb_sndbulkpipe(this_usbduxsub->usbdev,
					      COMMAND_OUT_EP),
			      this_usbduxsub->dux_commands, SIZEOFDUXBUFFER,
			      &nsent, BULK_TIMEOUT);
	if (result < 0)
		dev_err(&this_usbduxsub->interface->dev, "comedi%d: "
			"could not transmit dux_command to the usb-device, "
			"err=%d\n", this_usbduxsub->comedidev->minor, result);

	return result;
}

static int receive_dux_commands(struct usbduxsub *this_usbduxsub, int command)
{
	int result = (-EFAULT);
	int nrec;
	int i;

	for (i = 0; i < RETRIES; i++) {
		result = usb_bulk_msg(this_usbduxsub->usbdev,
				      usb_rcvbulkpipe(this_usbduxsub->usbdev,
						      COMMAND_IN_EP),
				      this_usbduxsub->insnBuffer, SIZEINSNBUF,
				      &nrec, BULK_TIMEOUT);
		if (result < 0) {
			dev_err(&this_usbduxsub->interface->dev, "comedi%d: "
				"insn: USB error %d "
				"while receiving DUX command"
				"\n", this_usbduxsub->comedidev->minor,
				result);
			return result;
		}
		if (this_usbduxsub->insnBuffer[0] == command)
			return result;
	}
	/* this is only reached if the data has been requested a couple of
	 * times */
	return -EFAULT;
}

static int usbdux_ai_inttrig(struct comedi_device *dev,
			     struct comedi_subdevice *s, unsigned int trignum)
{
	int ret;
	struct usbduxsub *this_usbduxsub = dev->private;
	if (!this_usbduxsub)
		return -EFAULT;

	down(&this_usbduxsub->sem);
	if (trignum != 0) {
		dev_err(&this_usbduxsub->interface->dev,
			"comedi%d: usbdux_ai_inttrig: invalid trignum\n",
			dev->minor);
		up(&this_usbduxsub->sem);
		return -EINVAL;
	}
	if (!this_usbduxsub->ai_cmd_running) {
		ret = usbduxsigma_submit_urbs(dev, this_usbduxsub->urbIn,
					      this_usbduxsub->numOfInBuffers,
					      1);
		if (ret < 0) {
			up(&this_usbduxsub->sem);
			return ret;
		}
		this_usbduxsub->ai_cmd_running = 1;
		s->async->inttrig = NULL;
	} else {
		dev_err(&this_usbduxsub->interface->dev,
			"comedi%d: ai_inttrig but acqu is already running\n",
			dev->minor);
	}
	up(&this_usbduxsub->sem);
	return 1;
}

static int usbdux_ai_cmd(struct comedi_device *dev, struct comedi_subdevice *s)
{
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int chan;
	int i, ret;
	struct usbduxsub *this_usbduxsub = dev->private;
	int result;
	uint8_t muxsg0 = 0;
	uint8_t muxsg1 = 0;
	uint8_t sysred = 0;

	if (!this_usbduxsub)
		return -EFAULT;

	/* block other CPUs from starting an ai_cmd */
	down(&this_usbduxsub->sem);
	if (this_usbduxsub->ai_cmd_running) {
		dev_err(&this_usbduxsub->interface->dev, "comedi%d: "
			"ai_cmd not possible. Another ai_cmd is running.\n",
			dev->minor);
		up(&this_usbduxsub->sem);
		return -EBUSY;
	}
	/* set current channel of the running acquisition to zero */
	s->async->cur_chan = 0;

	/* first the number of channels per time step */
	this_usbduxsub->dux_commands[1] = cmd->chanlist_len;

	/* CONFIG0 */
	this_usbduxsub->dux_commands[2] = 0x12;

	/* CONFIG1: 23kHz sampling rate, delay = 0us,  */
	this_usbduxsub->dux_commands[3] = 0x03;

	/* CONFIG3: differential channels off */
	this_usbduxsub->dux_commands[4] = 0x00;

	for (i = 0; i < cmd->chanlist_len; i++) {
		chan = CR_CHAN(cmd->chanlist[i]);
		create_adc_command(chan, &muxsg0, &muxsg1);
		if (i >= NUMCHANNELS) {
			dev_err(&this_usbduxsub->interface->dev,
				"comedi%d: channel list too long\n",
				dev->minor);
			break;
		}
	}
	this_usbduxsub->dux_commands[5] = muxsg0;
	this_usbduxsub->dux_commands[6] = muxsg1;
	this_usbduxsub->dux_commands[7] = sysred;

	result = send_dux_commands(this_usbduxsub, SENDADCOMMANDS);
	if (result < 0) {
		up(&this_usbduxsub->sem);
		return result;
	}

	if (this_usbduxsub->high_speed) {
		/*
		 * every 2 channels get a time window of 125us. Thus, if we
		 * sample all 16 channels we need 1ms. If we sample only one
		 * channel we need only 125us
		 */
		this_usbduxsub->ai_interval =
			chanToInterval(cmd->chanlist_len);
		this_usbduxsub->ai_timer = cmd->scan_begin_arg / (125000 *
							  (this_usbduxsub->
							   ai_interval));
	} else {
		/* interval always 1ms */
		this_usbduxsub->ai_interval = 1;
		this_usbduxsub->ai_timer = cmd->scan_begin_arg / 1000000;
	}
	if (this_usbduxsub->ai_timer < 1) {
		dev_err(&this_usbduxsub->interface->dev, "comedi%d: ai_cmd: "
			"timer=%d, scan_begin_arg=%d. "
			"Not properly tested by cmdtest?\n", dev->minor,
			this_usbduxsub->ai_timer, cmd->scan_begin_arg);
		up(&this_usbduxsub->sem);
		return -EINVAL;
	}
	this_usbduxsub->ai_counter = this_usbduxsub->ai_timer;

	if (cmd->stop_src == TRIG_COUNT) {
		/* data arrives as one packet */
		this_usbduxsub->ai_sample_count = cmd->stop_arg;
		this_usbduxsub->ai_continuous = 0;
	} else {
		/* continuous acquisition */
		this_usbduxsub->ai_continuous = 1;
		this_usbduxsub->ai_sample_count = 0;
	}

	if (cmd->start_src == TRIG_NOW) {
		/* enable this acquisition operation */
		ret = usbduxsigma_submit_urbs(dev, this_usbduxsub->urbIn,
					      this_usbduxsub->numOfInBuffers,
					      1);
		if (ret < 0) {
			up(&this_usbduxsub->sem);
			return ret;
		}
		this_usbduxsub->ai_cmd_running = 1;
		s->async->inttrig = NULL;
	} else {
		/* TRIG_INT */
		/* don't enable the acquision operation */
		/* wait for an internal signal */
		s->async->inttrig = usbdux_ai_inttrig;
	}
	up(&this_usbduxsub->sem);
	return 0;
}

/* Mode 0 is used to get a single conversion on demand */
static int usbdux_ai_insn_read(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       struct comedi_insn *insn, unsigned int *data)
{
	int i;
	int32_t one = 0;
	int chan;
	int err;
	struct usbduxsub *this_usbduxsub = dev->private;
	uint8_t muxsg0 = 0;
	uint8_t muxsg1 = 0;
	uint8_t sysred = 0;

	if (!this_usbduxsub)
		return 0;

	down(&this_usbduxsub->sem);
	if (this_usbduxsub->ai_cmd_running) {
		dev_err(&this_usbduxsub->interface->dev,
			"comedi%d: ai_insn_read not possible. "
			"Async Command is running.\n", dev->minor);
		up(&this_usbduxsub->sem);
		return 0;
	}

	/* sample one channel */
	/* CONFIG0: chopper on */
	this_usbduxsub->dux_commands[1] = 0x16;

	/* CONFIG1: 2kHz sampling rate */
	this_usbduxsub->dux_commands[2] = 0x80;

	/* CONFIG3: differential channels off */
	this_usbduxsub->dux_commands[3] = 0x00;

	chan = CR_CHAN(insn->chanspec);
	create_adc_command(chan, &muxsg0, &muxsg1);

	this_usbduxsub->dux_commands[4] = muxsg0;
	this_usbduxsub->dux_commands[5] = muxsg1;
	this_usbduxsub->dux_commands[6] = sysred;

	/* adc commands */
	err = send_dux_commands(this_usbduxsub, SENDSINGLEAD);
	if (err < 0) {
		up(&this_usbduxsub->sem);
		return err;
	}

	for (i = 0; i < insn->n; i++) {
		err = receive_dux_commands(this_usbduxsub, SENDSINGLEAD);
		if (err < 0) {
			up(&this_usbduxsub->sem);
			return 0;
		}
		/* 32 bits big endian from the A/D converter */
		one = be32_to_cpu(*((int32_t *)
				    ((this_usbduxsub->insnBuffer)+1)));
		/* mask out the status byte */
		one = one & 0x00ffffff;
		/* turn it into an unsigned integer */
		one = one ^ 0x00800000;
		data[i] = one;
	}
	up(&this_usbduxsub->sem);
	return i;
}




static int usbdux_getstatusinfo(struct comedi_device *dev, int chan)
{
	struct usbduxsub *this_usbduxsub = dev->private;
	uint8_t sysred = 0;
	uint32_t one;
	int err;

	if (!this_usbduxsub)
		return 0;

	if (this_usbduxsub->ai_cmd_running) {
		dev_err(&this_usbduxsub->interface->dev,
			"comedi%d: status read not possible. "
			"Async Command is running.\n", dev->minor);
		return 0;
	}

	/* CONFIG0 */
	this_usbduxsub->dux_commands[1] = 0x12;

	/* CONFIG1: 2kHz sampling rate */
	this_usbduxsub->dux_commands[2] = 0x80;

	/* CONFIG3: differential channels off */
	this_usbduxsub->dux_commands[3] = 0x00;

	if (chan == 1) {
		/* ADC offset */
		sysred = sysred | 1;
	} else if (chan == 2) {
		/* VCC */
		sysred = sysred | 4;
	} else if (chan == 3) {
		/* temperature */
		sysred = sysred | 8;
	} else if (chan == 4) {
		/* gain */
		sysred = sysred | 16;
	} else if (chan == 5) {
		/* ref */
		sysred = sysred | 32;
	}

	this_usbduxsub->dux_commands[4] = 0;
	this_usbduxsub->dux_commands[5] = 0;
	this_usbduxsub->dux_commands[6] = sysred;

	/* adc commands */
	err = send_dux_commands(this_usbduxsub, SENDSINGLEAD);
	if (err < 0)
		return err;

	err = receive_dux_commands(this_usbduxsub, SENDSINGLEAD);
	if (err < 0)
		return err;

	/* 32 bits big endian from the A/D converter */
	one = be32_to_cpu(*((int32_t *)((this_usbduxsub->insnBuffer)+1)));
	/* mask out the status byte */
	one = one & 0x00ffffff;
	one = one ^ 0x00800000;

	return (int)one;
}






/************************************/
/* analog out */

static int usbdux_ao_insn_read(struct comedi_device *dev,
			       struct comedi_subdevice *s,
			       struct comedi_insn *insn, unsigned int *data)
{
	int i;
	int chan = CR_CHAN(insn->chanspec);
	struct usbduxsub *this_usbduxsub = dev->private;

	if (!this_usbduxsub)
		return -EFAULT;

	down(&this_usbduxsub->sem);
	for (i = 0; i < insn->n; i++)
		data[i] = this_usbduxsub->outBuffer[chan];

	up(&this_usbduxsub->sem);
	return i;
}

static int usbdux_ao_insn_write(struct comedi_device *dev,
				struct comedi_subdevice *s,
				struct comedi_insn *insn, unsigned int *data)
{
	int i, err;
	int chan = CR_CHAN(insn->chanspec);
	struct usbduxsub *this_usbduxsub = dev->private;

	if (!this_usbduxsub)
		return -EFAULT;

	down(&this_usbduxsub->sem);
	if (this_usbduxsub->ao_cmd_running) {
		dev_err(&this_usbduxsub->interface->dev,
			"comedi%d: ao_insn_write: "
			"ERROR: asynchronous ao_cmd is running\n", dev->minor);
		up(&this_usbduxsub->sem);
		return 0;
	}

	for (i = 0; i < insn->n; i++) {
		dev_dbg(&this_usbduxsub->interface->dev,
			"comedi%d: ao_insn_write: data[chan=%d,i=%d]=%d\n",
			dev->minor, chan, i, data[i]);

		/* number of channels: 1 */
		this_usbduxsub->dux_commands[1] = 1;
		/* channel number */
		this_usbduxsub->dux_commands[2] = data[i];
		this_usbduxsub->outBuffer[chan] = data[i];
		this_usbduxsub->dux_commands[3] = chan;
		err = send_dux_commands(this_usbduxsub, SENDDACOMMANDS);
		if (err < 0) {
			up(&this_usbduxsub->sem);
			return err;
		}
	}
	up(&this_usbduxsub->sem);

	return i;
}

static int usbdux_ao_inttrig(struct comedi_device *dev,
			     struct comedi_subdevice *s, unsigned int trignum)
{
	int ret;
	struct usbduxsub *this_usbduxsub = dev->private;

	if (!this_usbduxsub)
		return -EFAULT;

	down(&this_usbduxsub->sem);
	if (trignum != 0) {
		dev_err(&this_usbduxsub->interface->dev,
			"comedi%d: usbdux_ao_inttrig: invalid trignum\n",
			dev->minor);
		ret = -EINVAL;
		goto out;
	}
	if (!(this_usbduxsub->ao_cmd_running)) {
		ret = usbduxsigma_submit_urbs(dev, this_usbduxsub->urbOut,
					      this_usbduxsub->numOfOutBuffers,
					      0);
		if (ret < 0)
			goto out;
		this_usbduxsub->ao_cmd_running = 1;
		s->async->inttrig = NULL;
	} else {
		dev_err(&this_usbduxsub->interface->dev,
			"comedi%d: ao_inttrig but acqu is already running.\n",
			dev->minor);
	}
	ret = 1;
out:
	up(&this_usbduxsub->sem);
	return ret;
}

static int usbdux_ao_cmdtest(struct comedi_device *dev,
			     struct comedi_subdevice *s,
			     struct comedi_cmd *cmd)
{
	struct usbduxsub *this_usbduxsub = dev->private;
	int err = 0;
	unsigned int flags;

	if (!this_usbduxsub)
		return -EFAULT;

	/* Step 1 : check if triggers are trivially valid */

	err |= cfc_check_trigger_src(&cmd->start_src, TRIG_NOW | TRIG_INT);

	if (0) {		/* (this_usbduxsub->high_speed) */
		/*
		 * start immediately a new scan
		 * the sampling rate is set by the coversion rate
		 */
		flags = TRIG_FOLLOW;
	} else {
		/* start a new scan (output at once) with a timer */
		flags = TRIG_TIMER;
	}
	err |= cfc_check_trigger_src(&cmd->scan_begin_src, flags);

	err |= cfc_check_trigger_src(&cmd->convert_src, TRIG_NOW);
	err |= cfc_check_trigger_src(&cmd->scan_end_src, TRIG_COUNT);
	err |= cfc_check_trigger_src(&cmd->stop_src, TRIG_COUNT | TRIG_NONE);

	if (err)
		return 1;

	/* Step 2a : make sure trigger sources are unique */

	err |= cfc_check_trigger_is_unique(cmd->start_src);
	err |= cfc_check_trigger_is_unique(cmd->stop_src);

	/* Step 2b : and mutually compatible */

	if (err)
		return 2;

	/* Step 3: check if arguments are trivially valid */

	err |= cfc_check_trigger_arg_is(&cmd->start_arg, 0);

	if (cmd->scan_begin_src == TRIG_FOLLOW)	/* internal trigger */
		err |= cfc_check_trigger_arg_is(&cmd->scan_begin_arg, 0);

	if (cmd->scan_begin_src == TRIG_TIMER)
		err |= cfc_check_trigger_arg_min(&cmd->scan_begin_arg,
						 1000000);

	/* not used now, is for later use */
	if (cmd->convert_src == TRIG_TIMER)
		err |= cfc_check_trigger_arg_min(&cmd->convert_arg, 125000);

	err |= cfc_check_trigger_arg_is(&cmd->scan_end_arg, cmd->chanlist_len);

	if (cmd->stop_src == TRIG_COUNT) {
		/* any count is allowed */
	} else {
		/* TRIG_NONE */
		err |= cfc_check_trigger_arg_is(&cmd->stop_arg, 0);
	}

	if (err)
		return 3;

	return 0;
}

static int usbdux_ao_cmd(struct comedi_device *dev, struct comedi_subdevice *s)
{
	struct comedi_cmd *cmd = &s->async->cmd;
	unsigned int chan, gain;
	int i, ret;
	struct usbduxsub *this_usbduxsub = dev->private;

	if (!this_usbduxsub)
		return -EFAULT;

	down(&this_usbduxsub->sem);
	/* set current channel of the running acquisition to zero */
	s->async->cur_chan = 0;
	for (i = 0; i < cmd->chanlist_len; ++i) {
		chan = CR_CHAN(cmd->chanlist[i]);
		gain = CR_RANGE(cmd->chanlist[i]);
		if (i >= NUMOUTCHANNELS) {
			dev_err(&this_usbduxsub->interface->dev,
				"comedi%d: %s: channel list too long\n",
				dev->minor, __func__);
			break;
		}
		this_usbduxsub->dac_commands[i] = chan;
		dev_dbg(&this_usbduxsub->interface->dev,
			"comedi%d: dac command for ch %d is %x\n",
			dev->minor, i, this_usbduxsub->dac_commands[i]);
	}

	/* we count in steps of 1ms (125us) */
	/* 125us mode not used yet */
	if (0) {		/* (this_usbduxsub->high_speed) */
		/* 125us */
		/* timing of the conversion itself: every 125 us */
		this_usbduxsub->ao_timer = cmd->convert_arg / 125000;
	} else {
		/* 1ms */
		/* timing of the scan: we get all channels at once */
		this_usbduxsub->ao_timer = cmd->scan_begin_arg / 1000000;
		dev_dbg(&this_usbduxsub->interface->dev,
			"comedi%d: scan_begin_src=%d, scan_begin_arg=%d, "
			"convert_src=%d, convert_arg=%d\n", dev->minor,
			cmd->scan_begin_src, cmd->scan_begin_arg,
			cmd->convert_src, cmd->convert_arg);
		dev_dbg(&this_usbduxsub->interface->dev,
			"comedi%d: ao_timer=%d (ms)\n",
			dev->minor, this_usbduxsub->ao_timer);
		if (this_usbduxsub->ao_timer < 1) {
			dev_err(&this_usbduxsub->interface->dev,
				"comedi%d: usbdux: ao_timer=%d, "
				"scan_begin_arg=%d. "
				"Not properly tested by cmdtest?\n",
				dev->minor, this_usbduxsub->ao_timer,
				cmd->scan_begin_arg);
			up(&this_usbduxsub->sem);
			return -EINVAL;
		}
	}
	this_usbduxsub->ao_counter = this_usbduxsub->ao_timer;

	if (cmd->stop_src == TRIG_COUNT) {
		/* not continuous */
		/* counter */
		/* high speed also scans everything at once */
		if (0) {	/* (this_usbduxsub->high_speed) */
			this_usbduxsub->ao_sample_count =
			    (cmd->stop_arg) * (cmd->scan_end_arg);
		} else {
			/* there's no scan as the scan has been */
			/* perf inside the FX2 */
			/* data arrives as one packet */
			this_usbduxsub->ao_sample_count = cmd->stop_arg;
		}
		this_usbduxsub->ao_continuous = 0;
	} else {
		/* continuous acquisition */
		this_usbduxsub->ao_continuous = 1;
		this_usbduxsub->ao_sample_count = 0;
	}

	if (cmd->start_src == TRIG_NOW) {
		/* enable this acquisition operation */
		ret = usbduxsigma_submit_urbs(dev, this_usbduxsub->urbOut,
					      this_usbduxsub->numOfOutBuffers,
					      0);
		if (ret < 0) {
			up(&this_usbduxsub->sem);
			return ret;
		}
		this_usbduxsub->ao_cmd_running = 1;
		s->async->inttrig = NULL;
	} else {
		/* TRIG_INT */
		/* submit the urbs later */
		/* wait for an internal signal */
		s->async->inttrig = usbdux_ao_inttrig;
	}

	up(&this_usbduxsub->sem);
	return 0;
}

static int usbdux_dio_insn_config(struct comedi_device *dev,
				  struct comedi_subdevice *s,
				  struct comedi_insn *insn, unsigned int *data)
{
	int chan = CR_CHAN(insn->chanspec);

	/* The input or output configuration of each digital line is
	 * configured by a special insn_config instruction.  chanspec
	 * contains the channel to be changed, and data[0] contains the
	 * value COMEDI_INPUT or COMEDI_OUTPUT. */

	switch (data[0]) {
	case INSN_CONFIG_DIO_OUTPUT:
		s->io_bits |= 1 << chan;	/* 1 means Out */
		break;
	case INSN_CONFIG_DIO_INPUT:
		s->io_bits &= ~(1 << chan);
		break;
	case INSN_CONFIG_DIO_QUERY:
		data[1] =
		    (s->io_bits & (1 << chan)) ? COMEDI_OUTPUT : COMEDI_INPUT;
		break;
	default:
		return -EINVAL;
		break;
	}
	/* we don't tell the firmware here as it would take 8 frames */
	/* to submit the information. We do it in the insn_bits. */
	return insn->n;
}

static int usbdux_dio_insn_bits(struct comedi_device *dev,
				struct comedi_subdevice *s,
				struct comedi_insn *insn,
				unsigned int *data)
{

	struct usbduxsub *this_usbduxsub = dev->private;
	int err;

	if (!this_usbduxsub)
		return -EFAULT;

	down(&this_usbduxsub->sem);

	/* The insn data is a mask in data[0] and the new data
	 * in data[1], each channel cooresponding to a bit. */
	s->state &= ~data[0];
	s->state |= data[0] & data[1];
	/* The commands are 8 bits wide */
	this_usbduxsub->dux_commands[1] = (s->io_bits) & 0x000000FF;
	this_usbduxsub->dux_commands[4] = (s->state) & 0x000000FF;
	this_usbduxsub->dux_commands[2] = ((s->io_bits) & 0x0000FF00) >> 8;
	this_usbduxsub->dux_commands[5] = ((s->state) & 0x0000FF00) >> 8;
	this_usbduxsub->dux_commands[3] = ((s->io_bits) & 0x00FF0000) >> 16;
	this_usbduxsub->dux_commands[6] = ((s->state) & 0x00FF0000) >> 16;

	/* This command also tells the firmware to return */
	/* the digital input lines */
	err = send_dux_commands(this_usbduxsub, SENDDIOBITSCOMMAND);
	if (err < 0) {
		up(&this_usbduxsub->sem);
		return err;
	}
	err = receive_dux_commands(this_usbduxsub, SENDDIOBITSCOMMAND);
	if (err < 0) {
		up(&this_usbduxsub->sem);
		return err;
	}

	data[1] = (((unsigned int)(this_usbduxsub->insnBuffer[1]))&0xff) |
		((((unsigned int)(this_usbduxsub->insnBuffer[2]))&0xff) << 8) |
		((((unsigned int)(this_usbduxsub->insnBuffer[3]))&0xff) << 16);

	s->state = data[1];

	up(&this_usbduxsub->sem);
	return insn->n;
}

static void usbdux_pwm_stop(struct usbduxsub *devpriv, int do_unlink)
{
	if (do_unlink) {
		if (devpriv->urbPwm)
			usb_kill_urb(devpriv->urbPwm);
	}

	devpriv->pwm_cmd_running = 0;
}

static int usbdux_pwm_cancel(struct comedi_device *dev,
			     struct comedi_subdevice *s)
{
	struct usbduxsub *devpriv = dev->private;

	/* unlink only if it is really running */
	usbdux_pwm_stop(devpriv, devpriv->pwm_cmd_running);

	return send_dux_commands(devpriv, SENDPWMOFF);
}

static void usbduxsub_pwm_irq(struct urb *urb)
{
	int ret;
	struct usbduxsub *this_usbduxsub;
	struct comedi_device *this_comedidev;
	struct comedi_subdevice *s;

	/* printk(KERN_DEBUG "PWM: IRQ\n"); */

	/* the context variable points to the subdevice */
	this_comedidev = urb->context;
	/* the private structure of the subdevice is struct usbduxsub */
	this_usbduxsub = this_comedidev->private;

	s = &this_comedidev->subdevices[SUBDEV_DA];

	switch (urb->status) {
	case 0:
		/* success */
		break;

	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -ECONNABORTED:
		/*
		 * after an unlink command, unplug, ... etc
		 * no unlink needed here. Already shutting down.
		 */
		if (this_usbduxsub->pwm_cmd_running)
			usbdux_pwm_stop(this_usbduxsub, 0);

		return;

	default:
		/* a real error */
		if (this_usbduxsub->pwm_cmd_running) {
			dev_err(&this_usbduxsub->interface->dev,
				"comedi_: Non-zero urb status received in "
				"pwm intr context: %d\n", urb->status);
			usbdux_pwm_stop(this_usbduxsub, 0);
		}
		return;
	}

	/* are we actually running? */
	if (!(this_usbduxsub->pwm_cmd_running))
		return;

	urb->transfer_buffer_length = this_usbduxsub->sizePwmBuf;
	urb->dev = this_usbduxsub->usbdev;
	urb->status = 0;
	if (this_usbduxsub->pwm_cmd_running) {
		ret = usb_submit_urb(urb, GFP_ATOMIC);
		if (ret < 0) {
			dev_err(&this_usbduxsub->interface->dev,
				"comedi_: pwm urb resubm failed in int-cont. "
				"ret=%d", ret);
			if (ret == EL2NSYNC)
				dev_err(&this_usbduxsub->interface->dev,
					"buggy USB host controller or bug in "
					"IRQ handling!\n");

			/* don't do an unlink here */
			usbdux_pwm_stop(this_usbduxsub, 0);
		}
	}
}

static int usbduxsub_submit_PwmURBs(struct usbduxsub *usbduxsub)
{
	int errFlag;

	if (!usbduxsub)
		return -EFAULT;

	/* in case of a resubmission after an unlink... */
	usb_fill_bulk_urb(usbduxsub->urbPwm,
			  usbduxsub->usbdev,
			  usb_sndbulkpipe(usbduxsub->usbdev, PWM_EP),
			  usbduxsub->urbPwm->transfer_buffer,
			  usbduxsub->sizePwmBuf, usbduxsub_pwm_irq,
			  usbduxsub->comedidev);

	errFlag = usb_submit_urb(usbduxsub->urbPwm, GFP_ATOMIC);
	if (errFlag) {
		dev_err(&usbduxsub->interface->dev,
			"comedi_: usbduxsigma: pwm: usb_submit_urb error %d\n",
			errFlag);
		return errFlag;
	}
	return 0;
}

static int usbdux_pwm_period(struct comedi_device *dev,
			     struct comedi_subdevice *s, unsigned int period)
{
	struct usbduxsub *this_usbduxsub = dev->private;
	int fx2delay = 255;

	if (period < MIN_PWM_PERIOD) {
		dev_err(&this_usbduxsub->interface->dev,
			"comedi%d: illegal period setting for pwm.\n",
			dev->minor);
		return -EAGAIN;
	} else {
		fx2delay = period / ((int)(6 * 512 * (1.0 / 0.033))) - 6;
		if (fx2delay > 255) {
			dev_err(&this_usbduxsub->interface->dev,
				"comedi%d: period %d for pwm is too low.\n",
				dev->minor, period);
			return -EAGAIN;
		}
	}
	this_usbduxsub->pwmDelay = fx2delay;
	this_usbduxsub->pwmPeriod = period;
	return 0;
}

/* is called from insn so there's no need to do all the sanity checks */
static int usbdux_pwm_start(struct comedi_device *dev,
			    struct comedi_subdevice *s)
{
	int ret, i;
	struct usbduxsub *this_usbduxsub = dev->private;

	if (this_usbduxsub->pwm_cmd_running) {
		/* already running */
		return 0;
	}

	this_usbduxsub->dux_commands[1] = ((uint8_t) this_usbduxsub->pwmDelay);
	ret = send_dux_commands(this_usbduxsub, SENDPWMON);
	if (ret < 0)
		return ret;

	/* initialise the buffer */
	for (i = 0; i < this_usbduxsub->sizePwmBuf; i++)
		((char *)(this_usbduxsub->urbPwm->transfer_buffer))[i] = 0;

	this_usbduxsub->pwm_cmd_running = 1;
	ret = usbduxsub_submit_PwmURBs(this_usbduxsub);
	if (ret < 0) {
		this_usbduxsub->pwm_cmd_running = 0;
		return ret;
	}
	return 0;
}

/* generates the bit pattern for PWM with the optional sign bit */
static int usbdux_pwm_pattern(struct comedi_device *dev,
			      struct comedi_subdevice *s, int channel,
			      unsigned int value, unsigned int sign)
{
	struct usbduxsub *this_usbduxsub = dev->private;
	int i, szbuf;
	char *pBuf;
	char pwm_mask;
	char sgn_mask;
	char c;

	if (!this_usbduxsub)
		return -EFAULT;

	/* this is the DIO bit which carries the PWM data */
	pwm_mask = (1 << channel);
	/* this is the DIO bit which carries the optional direction bit */
	sgn_mask = (16 << channel);
	/* this is the buffer which will be filled with the with bit */
	/* pattern for one period */
	szbuf = this_usbduxsub->sizePwmBuf;
	pBuf = (char *)(this_usbduxsub->urbPwm->transfer_buffer);
	for (i = 0; i < szbuf; i++) {
		c = *pBuf;
		/* reset bits */
		c = c & (~pwm_mask);
		/* set the bit as long as the index is lower than the value */
		if (i < value)
			c = c | pwm_mask;
		/* set the optional sign bit for a relay */
		if (!sign) {
			/* positive value */
			c = c & (~sgn_mask);
		} else {
			/* negative value */
			c = c | sgn_mask;
		}
		*(pBuf++) = c;
	}
	return 1;
}

static int usbdux_pwm_write(struct comedi_device *dev,
			    struct comedi_subdevice *s,
			    struct comedi_insn *insn, unsigned int *data)
{
	struct usbduxsub *this_usbduxsub = dev->private;

	if (!this_usbduxsub)
		return -EFAULT;

	if ((insn->n) != 1) {
		/*
		 * doesn't make sense to have more than one value here because
		 * it would just overwrite the PWM buffer a couple of times
		 */
		return -EINVAL;
	}

	/*
	 * the sign is set via a special INSN only, this gives us 8 bits for
	 * normal operation
	 * relay sign 0 by default
	 */
	return usbdux_pwm_pattern(dev, s, CR_CHAN(insn->chanspec), data[0], 0);
}

static int usbdux_pwm_read(struct comedi_device *x1,
			   struct comedi_subdevice *x2, struct comedi_insn *x3,
			   unsigned int *x4)
{
	/* not needed */
	return -EINVAL;
};

/* switches on/off PWM */
static int usbdux_pwm_config(struct comedi_device *dev,
			     struct comedi_subdevice *s,
			     struct comedi_insn *insn, unsigned int *data)
{
	struct usbduxsub *this_usbduxsub = dev->private;
	switch (data[0]) {
	case INSN_CONFIG_ARM:
		/* switch it on */
		/*
		 * if not zero the PWM is limited to a certain time which is
		 * not supported here
		 */
		if (data[1] != 0)
			return -EINVAL;
		return usbdux_pwm_start(dev, s);
	case INSN_CONFIG_DISARM:
		return usbdux_pwm_cancel(dev, s);
	case INSN_CONFIG_GET_PWM_STATUS:
		/*
		 * to check if the USB transmission has failed or in case PWM
		 * was limited to n cycles to check if it has terminated
		 */
		data[1] = this_usbduxsub->pwm_cmd_running;
		return 0;
	case INSN_CONFIG_PWM_SET_PERIOD:
		return usbdux_pwm_period(dev, s, data[1]);
	case INSN_CONFIG_PWM_GET_PERIOD:
		data[1] = this_usbduxsub->pwmPeriod;
		return 0;
	case INSN_CONFIG_PWM_SET_H_BRIDGE:
		/* value in the first byte and the sign in the second for a
		   relay */
		return usbdux_pwm_pattern(dev, s,
					  /* the channel number */
					  CR_CHAN(insn->chanspec),
					  /* actual PWM data */
					  data[1],
					  /* just a sign */
					  (data[2] != 0));
	case INSN_CONFIG_PWM_GET_H_BRIDGE:
		/* values are not kept in this driver, nothing to return */
		return -EINVAL;
	}
	return -EINVAL;
}

/* end of PWM */
/*****************************************************************/

static void tidy_up(struct usbduxsub *usbduxsub_tmp)
{
	int i;

	if (!usbduxsub_tmp)
		return;

	/* shows the usb subsystem that the driver is down */
	if (usbduxsub_tmp->interface)
		usb_set_intfdata(usbduxsub_tmp->interface, NULL);

	usbduxsub_tmp->probed = 0;

	if (usbduxsub_tmp->urbIn) {
		/* force unlink all urbs */
		usbdux_ai_stop(usbduxsub_tmp, 1);
		for (i = 0; i < usbduxsub_tmp->numOfInBuffers; i++) {
			kfree(usbduxsub_tmp->urbIn[i]->transfer_buffer);
			usbduxsub_tmp->urbIn[i]->transfer_buffer = NULL;
			usb_free_urb(usbduxsub_tmp->urbIn[i]);
			usbduxsub_tmp->urbIn[i] = NULL;
		}
		kfree(usbduxsub_tmp->urbIn);
		usbduxsub_tmp->urbIn = NULL;
	}
	if (usbduxsub_tmp->urbOut) {
		/* force unlink all urbs */
		usbdux_ao_stop(usbduxsub_tmp, 1);
		for (i = 0; i < usbduxsub_tmp->numOfOutBuffers; i++) {
			if (usbduxsub_tmp->urbOut[i]->transfer_buffer) {
				kfree(usbduxsub_tmp->
				      urbOut[i]->transfer_buffer);
				usbduxsub_tmp->urbOut[i]->transfer_buffer =
				    NULL;
			}
			if (usbduxsub_tmp->urbOut[i]) {
				usb_free_urb(usbduxsub_tmp->urbOut[i]);
				usbduxsub_tmp->urbOut[i] = NULL;
			}
		}
		kfree(usbduxsub_tmp->urbOut);
		usbduxsub_tmp->urbOut = NULL;
	}
	if (usbduxsub_tmp->urbPwm) {
		/* force unlink urb */
		usbdux_pwm_stop(usbduxsub_tmp, 1);
		kfree(usbduxsub_tmp->urbPwm->transfer_buffer);
		usbduxsub_tmp->urbPwm->transfer_buffer = NULL;
		usb_free_urb(usbduxsub_tmp->urbPwm);
		usbduxsub_tmp->urbPwm = NULL;
	}
	kfree(usbduxsub_tmp->inBuffer);
	usbduxsub_tmp->inBuffer = NULL;
	kfree(usbduxsub_tmp->insnBuffer);
	usbduxsub_tmp->insnBuffer = NULL;
	kfree(usbduxsub_tmp->outBuffer);
	usbduxsub_tmp->outBuffer = NULL;
	kfree(usbduxsub_tmp->dac_commands);
	usbduxsub_tmp->dac_commands = NULL;
	kfree(usbduxsub_tmp->dux_commands);
	usbduxsub_tmp->dux_commands = NULL;
}

static int usbduxsigma_attach_common(struct comedi_device *dev,
				     struct usbduxsub *uds)
{
	int ret;
	struct comedi_subdevice *s;
	int n_subdevs;
	int offset;

	down(&uds->sem);
	/* pointer back to the corresponding comedi device */
	uds->comedidev = dev;

	/* set number of subdevices */
	if (uds->high_speed)
		n_subdevs = 4;	/* with pwm */
	else
		n_subdevs = 3;	/* without pwm */
	ret = comedi_alloc_subdevices(dev, n_subdevs);
	if (ret) {
		up(&uds->sem);
		return ret;
	}
	/* private structure is also simply the usb-structure */
	dev->private = uds;
	/* the first subdevice is the A/D converter */
	s = &dev->subdevices[SUBDEV_AD];
	/* the URBs get the comedi subdevice */
	/* which is responsible for reading */
	/* this is the subdevice which reads data */
	dev->read_subdev = s;
	/* the subdevice receives as private structure the */
	/* usb-structure */
	s->private = NULL;
	/* analog input */
	s->type = COMEDI_SUBD_AI;
	/* readable and ref is to ground, 32 bit wide data! */
	s->subdev_flags = SDF_READABLE | SDF_GROUND |
		SDF_CMD_READ | SDF_LSAMPL;
	/* 16 A/D channels */
	s->n_chan = NUMCHANNELS;
	/* length of the channellist */
	s->len_chanlist = NUMCHANNELS;
	/* callback functions */
	s->insn_read = usbdux_ai_insn_read;
	s->do_cmdtest = usbdux_ai_cmdtest;
	s->do_cmd = usbdux_ai_cmd;
	s->cancel = usbdux_ai_cancel;
	/* max value from the A/D converter (24bit) */
	s->maxdata = 0x00FFFFFF;
	/* range table to convert to physical units */
	s->range_table = (&range_usbdux_ai_range);
	/* analog output subdevice */
	s = &dev->subdevices[SUBDEV_DA];
	/* analog out */
	s->type = COMEDI_SUBD_AO;
	/* backward pointer */
	dev->write_subdev = s;
	/* the subdevice receives as private structure the */
	/* usb-structure */
	s->private = NULL;
	/* are writable */
	s->subdev_flags = SDF_WRITABLE | SDF_GROUND | SDF_CMD_WRITE;
	/* 4 channels */
	s->n_chan = 4;
	/* length of the channellist */
	s->len_chanlist = 4;
	/* 8 bit resolution */
	s->maxdata = 0x00ff;
	/* unipolar range */
	s->range_table = &range_unipolar2_5;
	/* callback */
	s->do_cmdtest = usbdux_ao_cmdtest;
	s->do_cmd = usbdux_ao_cmd;
	s->cancel = usbdux_ao_cancel;
	s->insn_read = usbdux_ao_insn_read;
	s->insn_write = usbdux_ao_insn_write;
	/* digital I/O subdevice */
	s = &dev->subdevices[SUBDEV_DIO];
	s->type = COMEDI_SUBD_DIO;
	s->subdev_flags = SDF_READABLE | SDF_WRITABLE;
	/* 8 external and 16 internal channels */
	s->n_chan = 24;
	s->maxdata = 1;
	s->range_table = (&range_digital);
	s->insn_bits = usbdux_dio_insn_bits;
	s->insn_config = usbdux_dio_insn_config;
	/* we don't use it */
	s->private = NULL;
	if (uds->high_speed) {
		/* timer / pwm subdevice */
		s = &dev->subdevices[SUBDEV_PWM];
		s->type = COMEDI_SUBD_PWM;
		s->subdev_flags = SDF_WRITABLE | SDF_PWM_HBRIDGE;
		s->n_chan = 8;
		/* this defines the max duty cycle resolution */
		s->maxdata = uds->sizePwmBuf;
		s->insn_write = usbdux_pwm_write;
		s->insn_read = usbdux_pwm_read;
		s->insn_config = usbdux_pwm_config;
		usbdux_pwm_period(dev, s, PWM_DEFAULT_PERIOD);
	}
	/* finally decide that it's attached */
	uds->attached = 1;
	up(&uds->sem);
	offset = usbdux_getstatusinfo(dev, 0);
	if (offset < 0)
		dev_err(&uds->interface->dev,
			"Communication to USBDUXSIGMA failed! Check firmware and cabling.");
	dev_info(&uds->interface->dev,
		 "comedi%d: attached, ADC_zero = %x\n", dev->minor, offset);
	return 0;
}

static int usbduxsigma_auto_attach(struct comedi_device *dev,
				   unsigned long context_unused)
{
	struct usb_interface *uinterf = comedi_to_usb_interface(dev);
	struct usbduxsub *uds = usb_get_intfdata(uinterf);
	struct usb_device *usb = uds->usbdev;
	int ret;

	dev->private = uds;	/* This is temporary... */
	ret = comedi_load_firmware(dev, &usb->dev, FIRMWARE,
				   usbduxsigma_firmware_upload, 0);
	if (ret < 0) {
		dev->private = NULL;
		return ret;
	}

	dev->private = NULL;

	down(&start_stop_sem);
	if (!uds) {
		dev_err(dev->class_dev,
			"usbduxsigma: error: auto_attach failed, not connected\n");
		ret = -ENODEV;
	} else if (uds->attached) {
		dev_err(dev->class_dev,
		       "usbduxsigma: error: auto_attach failed, already attached\n");
		ret = -ENODEV;
	} else
		ret = usbduxsigma_attach_common(dev, uds);
	up(&start_stop_sem);
	return ret;
}

static void usbduxsigma_detach(struct comedi_device *dev)
{
	struct usbduxsub *usb = dev->private;

	if (usb) {
		down(&usb->sem);
		dev->private = NULL;
		usb->attached = 0;
		usb->comedidev = NULL;
		up(&usb->sem);
	}
}

static struct comedi_driver usbduxsigma_driver = {
	.driver_name	= "usbduxsigma",
	.module		= THIS_MODULE,
	.auto_attach	= usbduxsigma_auto_attach,
	.detach		= usbduxsigma_detach,
};

static int usbduxsigma_usb_probe(struct usb_interface *uinterf,
				 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(uinterf);
	struct device *dev = &uinterf->dev;
	int i;
	int index;

	down(&start_stop_sem);
	/* look for a free place in the usbdux array */
	index = -1;
	for (i = 0; i < NUMUSBDUX; i++) {
		if (!(usbduxsub[i].probed)) {
			index = i;
			break;
		}
	}

	/* no more space */
	if (index == -1) {
		dev_err(dev, "Too many usbduxsigma-devices connected.\n");
		up(&start_stop_sem);
		return -EMFILE;
	}

	sema_init(&(usbduxsub[index].sem), 1);
	/* save a pointer to the usb device */
	usbduxsub[index].usbdev = udev;

	/* save the interface itself */
	usbduxsub[index].interface = uinterf;
	/* get the interface number from the interface */
	usbduxsub[index].ifnum = uinterf->altsetting->desc.bInterfaceNumber;
	/* hand the private data over to the usb subsystem */
	/* will be needed for disconnect */
	usb_set_intfdata(uinterf, &(usbduxsub[index]));

	/* test if it is high speed (USB 2.0) */
	usbduxsub[index].high_speed =
	    (usbduxsub[index].usbdev->speed == USB_SPEED_HIGH);

	/* create space for the commands of the DA converter */
	usbduxsub[index].dac_commands = kzalloc(NUMOUTCHANNELS, GFP_KERNEL);
	if (!usbduxsub[index].dac_commands) {
		tidy_up(&(usbduxsub[index]));
		up(&start_stop_sem);
		return -ENOMEM;
	}
	/* create space for the commands going to the usb device */
	usbduxsub[index].dux_commands = kzalloc(SIZEOFDUXBUFFER, GFP_KERNEL);
	if (!usbduxsub[index].dux_commands) {
		tidy_up(&(usbduxsub[index]));
		up(&start_stop_sem);
		return -ENOMEM;
	}
	/* create space for the in buffer and set it to zero */
	usbduxsub[index].inBuffer = kzalloc(SIZEINBUF, GFP_KERNEL);
	if (!(usbduxsub[index].inBuffer)) {
		tidy_up(&(usbduxsub[index]));
		up(&start_stop_sem);
		return -ENOMEM;
	}
	/* create space of the instruction buffer */
	usbduxsub[index].insnBuffer = kzalloc(SIZEINSNBUF, GFP_KERNEL);
	if (!(usbduxsub[index].insnBuffer)) {
		tidy_up(&(usbduxsub[index]));
		up(&start_stop_sem);
		return -ENOMEM;
	}
	/* create space for the outbuffer */
	usbduxsub[index].outBuffer = kzalloc(SIZEOUTBUF, GFP_KERNEL);
	if (!(usbduxsub[index].outBuffer)) {
		tidy_up(&(usbduxsub[index]));
		up(&start_stop_sem);
		return -ENOMEM;
	}
	/* setting to alternate setting 3: enabling iso ep and bulk ep. */
	i = usb_set_interface(usbduxsub[index].usbdev,
			      usbduxsub[index].ifnum, 3);
	if (i < 0) {
		dev_err(dev, "comedi_: usbduxsigma%d: "
			"could not set alternate setting 3 in high speed.\n",
			index);
		tidy_up(&(usbduxsub[index]));
		up(&start_stop_sem);
		return -ENODEV;
	}
	if (usbduxsub[index].high_speed)
		usbduxsub[index].numOfInBuffers = NUMOFINBUFFERSHIGH;
	else
		usbduxsub[index].numOfInBuffers = NUMOFINBUFFERSFULL;

	usbduxsub[index].urbIn = kcalloc(usbduxsub[index].numOfInBuffers,
					 sizeof(struct urb *),
					 GFP_KERNEL);
	if (!(usbduxsub[index].urbIn)) {
		tidy_up(&(usbduxsub[index]));
		up(&start_stop_sem);
		return -ENOMEM;
	}
	for (i = 0; i < usbduxsub[index].numOfInBuffers; i++) {
		/* one frame: 1ms */
		usbduxsub[index].urbIn[i] = usb_alloc_urb(1, GFP_KERNEL);
		if (usbduxsub[index].urbIn[i] == NULL) {
			dev_err(dev, "comedi_: usbduxsigma%d: "
				"Could not alloc. urb(%d)\n", index, i);
			tidy_up(&(usbduxsub[index]));
			up(&start_stop_sem);
			return -ENOMEM;
		}
		usbduxsub[index].urbIn[i]->dev = usbduxsub[index].usbdev;
		/* will be filled later with a pointer to the comedi-device */
		/* and ONLY then the urb should be submitted */
		usbduxsub[index].urbIn[i]->context = NULL;
		usbduxsub[index].urbIn[i]->pipe =
		    usb_rcvisocpipe(usbduxsub[index].usbdev, ISOINEP);
		usbduxsub[index].urbIn[i]->transfer_flags = URB_ISO_ASAP;
		usbduxsub[index].urbIn[i]->transfer_buffer =
		    kzalloc(SIZEINBUF, GFP_KERNEL);
		if (!(usbduxsub[index].urbIn[i]->transfer_buffer)) {
			tidy_up(&(usbduxsub[index]));
			up(&start_stop_sem);
			return -ENOMEM;
		}
		usbduxsub[index].urbIn[i]->complete = usbduxsub_ai_IsocIrq;
		usbduxsub[index].urbIn[i]->number_of_packets = 1;
		usbduxsub[index].urbIn[i]->transfer_buffer_length = SIZEINBUF;
		usbduxsub[index].urbIn[i]->iso_frame_desc[0].offset = 0;
		usbduxsub[index].urbIn[i]->iso_frame_desc[0].length =
			SIZEINBUF;
	}

	/* out */
	if (usbduxsub[index].high_speed)
		usbduxsub[index].numOfOutBuffers = NUMOFOUTBUFFERSHIGH;
	else
		usbduxsub[index].numOfOutBuffers = NUMOFOUTBUFFERSFULL;

	usbduxsub[index].urbOut = kcalloc(usbduxsub[index].numOfOutBuffers,
					  sizeof(struct urb *), GFP_KERNEL);
	if (!(usbduxsub[index].urbOut)) {
		tidy_up(&(usbduxsub[index]));
		up(&start_stop_sem);
		return -ENOMEM;
	}
	for (i = 0; i < usbduxsub[index].numOfOutBuffers; i++) {
		/* one frame: 1ms */
		usbduxsub[index].urbOut[i] = usb_alloc_urb(1, GFP_KERNEL);
		if (usbduxsub[index].urbOut[i] == NULL) {
			dev_err(dev, "comedi_: usbduxsigma%d: "
				"Could not alloc. urb(%d)\n", index, i);
			tidy_up(&(usbduxsub[index]));
			up(&start_stop_sem);
			return -ENOMEM;
		}
		usbduxsub[index].urbOut[i]->dev = usbduxsub[index].usbdev;
		/* will be filled later with a pointer to the comedi-device */
		/* and ONLY then the urb should be submitted */
		usbduxsub[index].urbOut[i]->context = NULL;
		usbduxsub[index].urbOut[i]->pipe =
		    usb_sndisocpipe(usbduxsub[index].usbdev, ISOOUTEP);
		usbduxsub[index].urbOut[i]->transfer_flags = URB_ISO_ASAP;
		usbduxsub[index].urbOut[i]->transfer_buffer =
		    kzalloc(SIZEOUTBUF, GFP_KERNEL);
		if (!(usbduxsub[index].urbOut[i]->transfer_buffer)) {
			tidy_up(&(usbduxsub[index]));
			up(&start_stop_sem);
			return -ENOMEM;
		}
		usbduxsub[index].urbOut[i]->complete = usbduxsub_ao_IsocIrq;
		usbduxsub[index].urbOut[i]->number_of_packets = 1;
		usbduxsub[index].urbOut[i]->transfer_buffer_length =
			SIZEOUTBUF;
		usbduxsub[index].urbOut[i]->iso_frame_desc[0].offset = 0;
		usbduxsub[index].urbOut[i]->iso_frame_desc[0].length =
		    SIZEOUTBUF;
		if (usbduxsub[index].high_speed) {
			/* uframes */
			usbduxsub[index].urbOut[i]->interval = 8;
		} else {
			/* frames */
			usbduxsub[index].urbOut[i]->interval = 1;
		}
	}

	/* pwm */
	if (usbduxsub[index].high_speed) {
		/* max bulk ep size in high speed */
		usbduxsub[index].sizePwmBuf = 512;
		usbduxsub[index].urbPwm = usb_alloc_urb(0, GFP_KERNEL);
		if (usbduxsub[index].urbPwm == NULL) {
			dev_err(dev, "comedi_: usbduxsigma%d: "
				"Could not alloc. pwm urb\n", index);
			tidy_up(&(usbduxsub[index]));
			up(&start_stop_sem);
			return -ENOMEM;
		}
		usbduxsub[index].urbPwm->transfer_buffer =
		    kzalloc(usbduxsub[index].sizePwmBuf, GFP_KERNEL);
		if (!(usbduxsub[index].urbPwm->transfer_buffer)) {
			tidy_up(&(usbduxsub[index]));
			up(&start_stop_sem);
			return -ENOMEM;
		}
	} else {
		usbduxsub[index].urbPwm = NULL;
		usbduxsub[index].sizePwmBuf = 0;
	}

	usbduxsub[index].ai_cmd_running = 0;
	usbduxsub[index].ao_cmd_running = 0;
	usbduxsub[index].pwm_cmd_running = 0;

	/* we've reached the bottom of the function */
	usbduxsub[index].probed = 1;
	up(&start_stop_sem);

	return comedi_usb_auto_config(uinterf, &usbduxsigma_driver, 0);;
}

static void usbduxsigma_usb_disconnect(struct usb_interface *intf)
{
	struct usbduxsub *usbduxsub_tmp = usb_get_intfdata(intf);
	struct usb_device *udev = interface_to_usbdev(intf);

	if (!usbduxsub_tmp) {
		dev_err(&intf->dev,
			"comedi_: disconnect called with null pointer.\n");
		return;
	}
	if (usbduxsub_tmp->usbdev != udev) {
		dev_err(&intf->dev, "comedi_: BUG! wrong ptr!\n");
		return;
	}
	if (usbduxsub_tmp->ai_cmd_running)
		/* we are still running a command */
		usbdux_ai_stop(usbduxsub_tmp, 1);
	if (usbduxsub_tmp->ao_cmd_running)
		/* we are still running a command */
		usbdux_ao_stop(usbduxsub_tmp, 1);
	comedi_usb_auto_unconfig(intf);
	down(&start_stop_sem);
	down(&usbduxsub_tmp->sem);
	tidy_up(usbduxsub_tmp);
	up(&usbduxsub_tmp->sem);
	up(&start_stop_sem);
}

static const struct usb_device_id usbduxsigma_usb_table[] = {
	{ USB_DEVICE(0x13d8, 0x0020) },
	{ USB_DEVICE(0x13d8, 0x0021) },
	{ USB_DEVICE(0x13d8, 0x0022) },
	{ }
};
MODULE_DEVICE_TABLE(usb, usbduxsigma_usb_table);

static struct usb_driver usbduxsigma_usb_driver = {
	.name		= "usbduxsigma",
	.probe		= usbduxsigma_usb_probe,
	.disconnect	= usbduxsigma_usb_disconnect,
	.id_table	= usbduxsigma_usb_table,
};
module_comedi_usb_driver(usbduxsigma_driver, usbduxsigma_usb_driver);

MODULE_AUTHOR("Bernd Porr, BerndPorr@f2s.com");
MODULE_DESCRIPTION("Stirling/ITL USB-DUX SIGMA -- Bernd.Porr@f2s.com");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(FIRMWARE);
