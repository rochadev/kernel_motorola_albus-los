/*
 * net/dsa/dsa_priv.h - Hardware switch handling
 * Copyright (c) 2008-2009 Marvell Semiconductor
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __DSA_PRIV_H
#define __DSA_PRIV_H

#include <linux/phy.h>
#include <net/dsa.h>

struct dsa_slave_priv {
	/*
	 * The linux network interface corresponding to this
	 * switch port.
	 */
	struct net_device	*dev;

	/*
	 * Which switch this port is a part of, and the port index
	 * for this port.
	 */
	struct dsa_switch	*parent;
	u8			port;

	/*
	 * The phylib phy_device pointer for the PHY connected
	 * to this port.
	 */
	struct phy_device	*phy;
};

/* dsa.c */
extern char dsa_driver_version[];

/* slave.c */
void dsa_slave_mii_bus_init(struct dsa_switch *ds);
struct net_device *dsa_slave_create(struct dsa_switch *ds,
				    struct device *parent,
				    int port, char *name);

/* tag_dsa.c */
extern const struct dsa_device_ops dsa_netdev_ops;

/* tag_edsa.c */
extern const struct dsa_device_ops edsa_netdev_ops;

/* tag_trailer.c */
extern const struct dsa_device_ops trailer_netdev_ops;


#endif
