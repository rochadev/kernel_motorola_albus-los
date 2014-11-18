/*
 * Greybus driver and device API
 *
 * Copyright 2014 Google Inc.
 *
 * Released under the GPLv2 only.
 */

#ifndef __LINUX_GREYBUS_H
#define __LINUX_GREYBUS_H

#ifdef __KERNEL__

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/idr.h>

#include "kernel_ver.h"
#include "greybus_id.h"
#include "greybus_manifest.h"
#include "manifest.h"
#include "module.h"
#include "interface.h"
#include "connection.h"
#include "protocol.h"
#include "operation.h"


/* Matches up with the Greybus Protocol specification document */
#define GREYBUS_VERSION_MAJOR	0x00
#define GREYBUS_VERSION_MINOR	0x01

#define GREYBUS_DEVICE_ID_MATCH_DEVICE \
	(GREYBUS_DEVICE_ID_MATCH_VENDOR | GREYBUS_DEVICE_ID_MATCH_PRODUCT)

#define GREYBUS_DEVICE(v, p)					\
	.match_flags	= GREYBUS_DEVICE_ID_MATCH_DEVICE,	\
	.vendor		= (v),					\
	.product	= (p),

#define GREYBUS_DEVICE_SERIAL(s)				\
	.match_flags	= GREYBUS_DEVICE_ID_MATCH_SERIAL,	\
	.serial_number	= (s),

/* XXX I couldn't get my Kconfig file to be noticed for out-of-tree build */
#ifndef CONFIG_HOST_DEV_CPORT_ID_MAX
#define CONFIG_HOST_DEV_CPORT_ID_MAX 128
#endif /* !CONFIG_HOST_DEV_CPORT_ID_MAX */

/* Maximum number of CPorts usable by a host device */
/* XXX This should really be determined by the AP module manifest */
#define HOST_DEV_CPORT_ID_MAX	CONFIG_HOST_DEV_CPORT_ID_MAX
#define CPORT_ID_BAD		U16_MAX		/* UniPro max id is 4095 */

/* For SP1 hardware, we are going to "hardcode" each device to have all logical
 * blocks in order to be able to address them as one unified "unit".  Then
 * higher up layers will then be able to talk to them as one logical block and
 * properly know how they are hooked together (i.e. which i2c port is on the
 * same module as the gpio pins, etc.)
 *
 * So, put the "private" data structures here in greybus.h and link to them off
 * of the "main" gb_module structure.
 */

struct greybus_host_device;
struct svc_msg;
struct gbuf;

/* Greybus "Host driver" structure, needed by a host controller driver to be
 * able to handle both SVC control as well as "real" greybus messages
 */
struct greybus_host_driver {
	size_t	hd_priv_size;

	int (*alloc_gbuf_data)(struct gbuf *gbuf, unsigned int size,
					gfp_t gfp_mask);
	void (*free_gbuf_data)(struct gbuf *gbuf);
	int (*submit_svc)(struct svc_msg *svc_msg,
			    struct greybus_host_device *hd);
	int (*submit_gbuf)(struct gbuf *gbuf, gfp_t gfp_mask);
	void (*kill_gbuf)(struct gbuf *gbuf);
};

struct greybus_host_device {
	struct kref kref;
	struct device *parent;
	const struct greybus_host_driver *driver;

	struct list_head modules;
	struct list_head connections;
	struct ida cport_id_map;
	u8 device_id;

	/* Private data for the host driver */
	unsigned long hd_priv[0] __aligned(sizeof(s64));
};

struct greybus_host_device *greybus_create_hd(struct greybus_host_driver *hd,
					      struct device *parent);
void greybus_remove_hd(struct greybus_host_device *hd);

struct greybus_driver {
	const char *name;

	int (*probe)(struct gb_module *gmod,
		     const struct greybus_module_id *id);
	void (*disconnect)(struct gb_module *gmod);

	int (*suspend)(struct gb_module *gmod, pm_message_t message);
	int (*resume)(struct gb_module *gmod);

	const struct greybus_module_id *id_table;

	struct device_driver driver;
};
#define to_greybus_driver(d) container_of(d, struct greybus_driver, driver)

/* Don't call these directly, use the module_greybus_driver() macro instead */
int greybus_register_driver(struct greybus_driver *driver,
			    struct module *module, const char *mod_name);
void greybus_deregister(struct greybus_driver *driver);

/* define to get proper THIS_MODULE and KBUILD_MODNAME values */
#define greybus_register(driver) \
	greybus_register_driver(driver, THIS_MODULE, KBUILD_MODNAME)

/**
 * module_greybus_driver() - Helper macro for registering a Greybus driver
 * @__greybus_driver: greybus_driver structure
 *
 * Helper macro for Greybus drivers to set up proper module init / exit
 * functions.  Replaces module_init() and module_exit() and keeps people from
 * printing pointless things to the kernel log when their driver is loaded.
 */
#define module_greybus_driver(__greybus_driver)	\
	module_driver(__greybus_driver, greybus_register, greybus_deregister)

int greybus_disabled(void);

/* Internal functions to gb module, move to internal .h file eventually. */

void gb_add_module(struct greybus_host_device *hd, u8 module_id,
		   u8 *data, int size);
void gb_remove_module(struct greybus_host_device *hd, u8 module_id);
void gb_remove_modules(struct greybus_host_device *hd);

int greybus_svc_in(struct greybus_host_device *hd, u8 *data, int length);
int gb_ap_init(void);
void gb_ap_exit(void);
int gb_debugfs_init(void);
void gb_debugfs_cleanup(void);
int gb_gbuf_init(void);
void gb_gbuf_exit(void);

extern struct bus_type greybus_bus_type;
extern const struct attribute_group *greybus_module_groups[];

int gb_uart_device_init(struct gb_connection *connection);
void gb_uart_device_exit(struct gb_connection *connection);

int svc_set_route_send(struct gb_interface *interface,
			       struct greybus_host_device *hd);

extern struct device_type greybus_module_type;
extern struct device_type greybus_interface_type;
extern struct device_type greybus_connection_type;

static inline int is_gb_module(const struct device *dev)
{
	return dev->type == &greybus_module_type;
}

static inline int is_gb_interface(const struct device *dev)
{
	return dev->type == &greybus_interface_type;
}

static inline int is_gb_connection(const struct device *dev)
{
	return dev->type == &greybus_connection_type;
}

#endif /* __KERNEL__ */
#endif /* __LINUX_GREYBUS_H */
