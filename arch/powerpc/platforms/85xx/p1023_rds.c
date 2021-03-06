/*
 * Copyright 2010-2011 Freescale Semiconductor, Inc.
 *
 * Author: Roy Zang <tie-fei.zang@freescale.com>
 *
 * Description:
 * P1023 RDS Board Setup
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/fsl_devices.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>

#include <asm/system.h>
#include <asm/time.h>
#include <asm/machdep.h>
#include <asm/pci-bridge.h>
#include <mm/mmu_decl.h>
#include <asm/prom.h>
#include <asm/udbg.h>
#include <asm/mpic.h>

#include <sysdev/fsl_soc.h>
#include <sysdev/fsl_pci.h>

/* ************************************************************************
 *
 * Setup the architecture
 *
 */
#ifdef CONFIG_SMP
void __init mpc85xx_smp_init(void);
#endif

static void __init mpc85xx_rds_setup_arch(void)
{
	struct device_node *np;

	if (ppc_md.progress)
		ppc_md.progress("p1023_rds_setup_arch()", 0);

	/* Map BCSR area */
	np = of_find_node_by_name(NULL, "bcsr");
	if (np != NULL) {
		static u8 __iomem *bcsr_regs;

		bcsr_regs = of_iomap(np, 0);
		of_node_put(np);

		if (!bcsr_regs) {
			printk(KERN_ERR
			       "BCSR: Failed to map bcsr register space\n");
			return;
		} else {
#define BCSR15_I2C_BUS0_SEG_CLR		0x07
#define BCSR15_I2C_BUS0_SEG2		0x02
/*
 * Note: Accessing exclusively i2c devices.
 *
 * The i2c controller selects initially ID EEPROM in the u-boot;
 * but if menu configuration selects RTC support in the kernel,
 * the i2c controller switches to select RTC chip in the kernel.
 */
#ifdef CONFIG_RTC_CLASS
			/* Enable RTC chip on the segment #2 of i2c */
			clrbits8(&bcsr_regs[15], BCSR15_I2C_BUS0_SEG_CLR);
			setbits8(&bcsr_regs[15], BCSR15_I2C_BUS0_SEG2);
#endif

			iounmap(bcsr_regs);
		}
	}

#ifdef CONFIG_PCI
	for_each_node_by_type(np, "pci")
		fsl_pci_setup(np);
#endif

#ifdef CONFIG_SMP
	mpc85xx_smp_init();
#endif
}

static struct of_device_id __initdata p1023_pci_ids[] = {
	{ .compatible = "fsl,p1023-pcie", },
	{},
};

static int __init p1023_rds_publish_pci_device(void)
{
	return of_platform_bus_probe(NULL, p1023_pci_ids, NULL);
}
machine_arch_initcall(p1023_rds, p1023_rds_publish_pci_device);

static struct of_device_id p1023_ids[] = {
	{ .type = "soc", },
	{ .compatible = "soc", },
	{ .compatible = "fsl,dpaa", },
	{ .compatible = "simple-bus", },
	{},
};


static int __init p1023_publish_devices(void)
{
	of_platform_bus_probe(NULL, p1023_ids, NULL);

	return 0;
}

machine_device_initcall(p1023_rds, p1023_publish_devices);

static void __init mpc85xx_rds_pic_init(void)
{
	struct mpic *mpic;
	struct device_node *np = NULL;

	np = of_find_node_by_type(NULL, "open-pic");
	if (!np) {
		printk(KERN_ERR "Could not find open-pic node\n");
		return;
	}

	mpic = mpic_alloc(np, 0,
		MPIC_PRIMARY | MPIC_WANTS_RESET | MPIC_BIG_ENDIAN |
		MPIC_BROKEN_FRR_NIRQS | MPIC_SINGLE_DEST_CPU,
		0, 256, " OpenPIC  ");

	BUG_ON(mpic == NULL);
	of_node_put(np);

	mpic_init(mpic);
}

static int __init p1023_rds_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	return of_flat_dt_is_compatible(root, "fsl,P1023RDS");

}

/* Early setup is required for large chunks of contiguous (and coarsely-aligned)
 * memory. The following shoe-horns Qman/Bman "init_early" calls into the
 * platform setup to let them parse their CCSR nodes early on. */
#ifdef CONFIG_FSL_QMAN_CONFIG
void __init qman_init_early(void);
#endif
#ifdef CONFIG_FSL_BMAN_CONFIG
void __init bman_init_early(void);
#endif

__init void p1023_rds_init_early(void)
{
#ifdef CONFIG_FSL_QMAN_CONFIG
	qman_init_early();
#endif
#ifdef CONFIG_FSL_BMAN_CONFIG
	bman_init_early();
#endif
}

define_machine(p1023_rds) {
	.name			= "P1023 RDS",
	.probe			= p1023_rds_probe,
	.setup_arch		= mpc85xx_rds_setup_arch,
	.init_IRQ		= mpc85xx_rds_pic_init,
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
	.init_early		= p1023_rds_init_early,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
};

