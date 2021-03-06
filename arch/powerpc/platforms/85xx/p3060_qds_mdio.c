/*
 * Provides QIXIS based muxing for the MDIO buses on the QDS boards
 *
 * Copyright (c) 2011 Freescale Semiconductor, Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/crc32.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>

#define QIXIS_BRDCFG4_OFFSET	0x54
#define BRDCFG4_EMI1_SEL_MASK	0xf0

struct p3060qds_mdio {
	struct mii_bus *real_bus;
	u8 *qixis_brdcfg4;
	u8 value;
	u8 mask;
};

/* Set the BRDCFG4 qixis register, and then write the MDIO regs */
int p3060qds_mdio_write(struct mii_bus *bus, int port_addr, int dev_addr,
			int regnum, u16 value)
{
	struct p3060qds_mdio *priv = bus->priv;

	/* Write BRDCFG4 to select this bus */
	clrsetbits_8(priv->qixis_brdcfg4, priv->mask, priv->value);

	/* Write through to the attached MDIO bus */
	return priv->real_bus->write(priv->real_bus, port_addr, dev_addr,
					regnum, value);
}

/* Set the BRDCFG4 qixis register, and then read from the MDIO bus */
int p3060qds_mdio_read(struct mii_bus *bus, int port_addr, int dev_addr,
			int regnum)
{
	struct p3060qds_mdio *priv = bus->priv;
	/* Write BRDCFG4 to select this bus */
	clrsetbits_8(priv->qixis_brdcfg4, priv->mask, priv->value);

	return priv->real_bus->read(priv->real_bus, port_addr, dev_addr,
					regnum);
}


/* Reset the MIIM registers, and wait for the bus to free */
static int p3060qds_mdio_reset(struct mii_bus *bus)
{
	struct p3060qds_mdio *priv = bus->priv;

	mutex_lock(&bus->mdio_lock);
	priv->real_bus->reset(priv->real_bus);
	mutex_unlock(&bus->mdio_lock);

	return 0;
}

static struct of_device_id p3060qds_qixis_match[] = {
	{
		.compatible = "fsl,p3060qds-fpga",
	},
	{}
};

static int p3060qds_mdio_probe(struct platform_device *ofdev)
{
	struct device_node *np = ofdev->dev.of_node;
	struct mii_bus *new_bus;
	struct p3060qds_mdio *priv;
	struct device_node *mdio, *qixis;
	struct platform_device *ofmdiodev;
	const u32 *addr;
	const u32 *val;
	u64 reg;
	int i;
	int err = 0;

	new_bus = mdiobus_alloc();
	if (NULL == new_bus)
		return -ENOMEM;

	new_bus->name = "Freescale P3060QDS MDIO Bus",
	new_bus->read = &p3060qds_mdio_read,
	new_bus->write = &p3060qds_mdio_write,
	new_bus->reset = &p3060qds_mdio_reset,

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		err = -ENOMEM;
		goto err_priv_alloc;
	}

	new_bus->priv = priv;

	/* Get the parent (real) mdio bus */
	mdio = of_parse_phandle(np, "fsl,mdio-handle", 0);

	if (mdio == NULL) {
		printk(KERN_ERR "Could not find real MDIO bus for %s\n",
			new_bus->id);
		err = -ENODEV;
		goto err_no_mdio_node;
	}

	ofmdiodev = of_find_device_by_node(mdio);

	if (!ofmdiodev) {
		printk(KERN_ERR "No of_device for MDIO node %s\n",
		       mdio->full_name);
		err = -ENODEV;
		goto err_no_mdio_dev;
	}

	of_node_put(mdio);

	priv->real_bus = dev_get_drvdata(&ofmdiodev->dev);

	if (!priv->real_bus) {
		printk(KERN_ERR "The MDIO bus has no ofdev!\n");
		err = -ENODEV;
		goto err_no_ofdev;
	}

	new_bus->irq = kcalloc(PHY_MAX_ADDR, sizeof(int), GFP_KERNEL);

	if (NULL == new_bus->irq) {
		err = -ENOMEM;
		goto err_irq_alloc;
	}

	for (i = 0; i < PHY_MAX_ADDR; i++)
		new_bus->irq[i] = PHY_POLL;

	new_bus->parent = &ofdev->dev;
	dev_set_drvdata(&ofdev->dev, new_bus);

	/* Find the qixis node */
	qixis = of_find_matching_node(NULL, p3060qds_qixis_match);
	if (!qixis) {
		err = -ENODEV;
		goto err_no_qixis;
	}

	addr = of_get_address(qixis, 0, NULL, NULL);
	if (!addr) {
		err = -ENODEV;
		goto err_no_qixis_addr;
	}

	reg = of_translate_address(qixis, addr);
	of_node_put(qixis);

	/* Map only the register we need to select the mdio bus (BRDCFG4) */
	priv->qixis_brdcfg4 = ioremap(reg + QIXIS_BRDCFG4_OFFSET,
				      sizeof(*priv->qixis_brdcfg4));
	if (!priv->qixis_brdcfg4) {
		err = -ENOMEM;
		goto err_ioremap;
	}

	val = of_get_property(np, "fsl,mdio-muxval", NULL);
	if (!val) {
		printk(KERN_ERR "No mux value found for %s\n", np->full_name);
		err = -ENODEV;
		goto err_get_muxval;
	}

	priv->mask = BRDCFG4_EMI1_SEL_MASK;
	priv->value = (*val) << 4;

	sprintf(new_bus->id, "%s@%d", np->name, *val);

	err = of_mdiobus_register(new_bus, np);

	if (err) {
		printk(KERN_ERR "%s: Cannot register as MDIO bus\n",
				new_bus->name);
		goto err_registration;
	}

	return 0;

err_get_muxval:
	iounmap(priv->qixis_brdcfg4);
err_ioremap:
err_no_qixis_addr:
	of_node_put(qixis);
err_no_qixis:
err_registration:
	kfree(new_bus->irq);
err_irq_alloc:
err_no_ofdev:
err_no_mdio_dev:
err_no_mdio_node:
	kfree(priv);
err_priv_alloc:
	mdiobus_free(new_bus);

	return err;
}


static int p3060qds_mdio_remove(struct platform_device *ofdev)
{
	struct device *device = &ofdev->dev;
	struct mii_bus *bus = dev_get_drvdata(device);
	struct p3060qds_mdio *priv = bus->priv;

	mdiobus_unregister(bus);

	dev_set_drvdata(device, NULL);

	iounmap(priv->qixis_brdcfg4);
	kfree(bus->irq);
	kfree(bus->priv);

	mdiobus_free(bus);

	return 0;
}

static struct of_device_id p3060qds_mdio_match[] = {
	{
		.compatible = "fsl,p3060qds-mdio",
	},
	{}
};

static struct platform_driver p3060qds_mdio_driver = {
	.driver = {
		.name = "p3060qds_mdio",
		.of_match_table = p3060qds_mdio_match,
	},
	.probe = p3060qds_mdio_probe,
	.remove = p3060qds_mdio_remove,
};

int __init p3060qds_mdio_init(void)
{
	return platform_driver_register(&p3060qds_mdio_driver);
}

void p3060qds_mdio_exit(void)
{
	platform_driver_unregister(&p3060qds_mdio_driver);
}
subsys_initcall_sync(p3060qds_mdio_init);
module_exit(p3060qds_mdio_exit);
