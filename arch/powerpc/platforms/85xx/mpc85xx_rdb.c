/*
 * MPC85xx RDB Board Setup
 *
 * Copyright 2009,2012 Freescale Semiconductor Inc.
 *
 * This program is free software; you can redistribute	it and/or modify it
 * under  the terms of	the GNU General	 Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/stddef.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/kdev_t.h>
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/interrupt.h>
#include <linux/of_platform.h>

#include <asm/system.h>
#include <asm/time.h>
#include <asm/machdep.h>
#include <asm/pci-bridge.h>
#include <mm/mmu_decl.h>
#include <asm/prom.h>
#include <asm/udbg.h>
#include <asm/mpic.h>
#include <asm/qe.h>
#include <asm/qe_ic.h>
#include <asm/fsl_guts.h>

#include <sysdev/fsl_soc.h>
#include <sysdev/fsl_pci.h>

#undef DEBUG

#ifdef DEBUG
#define DBG(fmt, args...) printk(KERN_ERR "%s: " fmt, __func__, ## args)
#else
#define DBG(fmt, args...)
#endif


void __init mpc85xx_rdb_pic_init(void)
{
	struct mpic *mpic;
	struct device_node *np;
	unsigned long root = of_get_flat_dt_root();

	np = of_find_node_by_type(NULL, "open-pic");
	if (np == NULL) {
		printk(KERN_ERR "Could not find open-pic node\n");
		return;
	}

	if (of_flat_dt_is_compatible(root, "fsl,MPC85XXRDB-CAMP")) {
		mpic = mpic_alloc(np, 0,
			MPIC_PRIMARY |
			MPIC_BIG_ENDIAN | MPIC_BROKEN_FRR_NIRQS |
			MPIC_SINGLE_DEST_CPU,
			0, 256, " OpenPIC  ");
	} else {
		mpic = mpic_alloc(np, 0,
		  MPIC_PRIMARY | MPIC_WANTS_RESET |
		  MPIC_BIG_ENDIAN | MPIC_BROKEN_FRR_NIRQS |
		  MPIC_SINGLE_DEST_CPU,
		  0, 256, " OpenPIC  ");
	}

	BUG_ON(mpic == NULL);
	of_node_put(np);

	mpic_init(mpic);

#ifdef CONFIG_QUICC_ENGINE
	np = of_find_compatible_node(NULL, NULL, "fsl,qe-ic");
	if (np) {
		qe_ic_init(np, 0, qe_ic_cascade_low_mpic,
				qe_ic_cascade_high_mpic);
		of_node_put(np);

	} else
		pr_err("%s: Could not find qe-ic node\n", __func__);
#endif
}

/*
 * Setup the architecture
 */
#ifdef CONFIG_SMP
extern void __init mpc85xx_smp_init(void);
#endif
static void __init mpc85xx_rdb_setup_arch(void)
{
#if defined(CONFIG_PCI) || defined(CONFIG_QUICC_ENGINE)
	struct device_node *np;
#endif

#if defined(CONFIG_QUICC_ENGINE) && defined(CONFIG_SPI_FSL_SPI)
	struct device_node *qe_spi;
#endif
	struct ccsr_guts __iomem *guts;

	if (ppc_md.progress)
		ppc_md.progress("mpc85xx_rdb_setup_arch()", 0);

#ifdef CONFIG_PCI
	for_each_node_by_type(np, "pci")
		fsl_pci_setup(np);
#endif

#ifdef CONFIG_SMP
	mpc85xx_smp_init();
#endif

#ifdef CONFIG_QUICC_ENGINE
	np = of_find_compatible_node(NULL, NULL, "fsl,qe");
	if (!np) {
		pr_err("%s: Could not find Quicc Engine node\n", __func__);
		goto qe_fail;
	}

	qe_reset();
	of_node_put(np);

	np = of_find_node_by_name(NULL, "par_io");
	if (np) {
		struct device_node *ucc;

		par_io_init(np);
		of_node_put(np);

		for_each_node_by_name(ucc, "ucc")
			par_io_of_config(ucc);

		/* To P1025 QE/TDM, the name of ucc nodes is "tdm@xxxx" */
		for_each_node_by_name(ucc, "tdm")
			par_io_of_config(ucc);
#ifdef CONFIG_SPI_FSL_SPI
		for_each_node_by_name(qe_spi, "spi")
			par_io_of_config(qe_spi);
#endif	/* CONFIG_SPI_FSL_SPI */
	}

	np = of_find_node_by_name(NULL, "global-utilities");
	if (np) {
		guts = of_iomap(np, 0);
		if (!guts)
			pr_err("mpc85xx-rdb: could not map global "
					"utilities register\n");
		else {
#if defined(CONFIG_UCC_GETH) || defined(CONFIG_SERIAL_QE)
			if (machine_is(p1025_rdb)) {
				/*
				 * P1025 has pins muxed for QE and other
				 * functions. To enable QE UEC mode, we
				 * need to set bit QE0 for UCC1 in Eth mode,
				 * QE0 and QE3 for UCC5 in Eth mode, QE9
				 * and QE12 for QE MII management singals
				 * in PMUXCR register.
				 */
				setbits32(&guts->pmuxcr, MPC85xx_PMUXCR_QE(0) |
						MPC85xx_PMUXCR_QE(3) |
						MPC85xx_PMUXCR_QE(9) |
						MPC85xx_PMUXCR_QE(12));
			}
#endif

#ifdef CONFIG_FSL_UCC_TDM
			if (machine_is(p1021_rdb_pc) || machine_is(p1025_rdb)) {

				/* Clear QE12 for releasing the LBCTL */
				clrbits32(&guts->pmuxcr, MPC85xx_PMUXCR_QE(12));
				/* TDMA */
				setbits32(&guts->pmuxcr, MPC85xx_PMUXCR_QE(5) |
						  MPC85xx_PMUXCR_QE(11));
				/* TDMB */
				setbits32(&guts->pmuxcr, MPC85xx_PMUXCR_QE(0) |
						  MPC85xx_PMUXCR_QE(9));
				/* TDMC */
				setbits32(&guts->pmuxcr, MPC85xx_PMUXCR_QE(0));
				/* TDMD */
				setbits32(&guts->pmuxcr, MPC85xx_PMUXCR_QE(8) |
						  MPC85xx_PMUXCR_QE(7));
			}
#endif	/* CONFIG_FSL_UCC_TDM */

#ifdef CONFIG_SPI_FSL_SPI
		if (of_find_compatible_node(NULL, NULL, "fsl,mpc8569-qe-spi")) {
			clrbits32(&guts->pmuxcr, MPC85xx_PMUXCR_QE(12));
			/*QE-SPI*/
			setbits32(&guts->pmuxcr, MPC85xx_PMUXCR_QE(6) |
					  MPC85xx_PMUXCR_QE(9) |
					  MPC85xx_PMUXCR_QE(10));
		}
#endif	/* CONFIG_SPI_FSL_SPI */
			iounmap(guts);
		}
		of_node_put(np);
	}
qe_fail:
#endif	/* CONFIG_QUICC_ENGINE */

	printk(KERN_INFO "MPC85xx RDB board from Freescale Semiconductor\n");
}

static struct of_device_id __initdata mpc85xxrdb_pci_ids[] = {
	{ .compatible = "fsl,mpc8548-pcie", },
	{},
};

static int __init mpc85xxrdb_publish_pci_device(void)
{
	return of_platform_bus_probe(NULL, mpc85xxrdb_pci_ids, NULL);
}
machine_arch_initcall(p2020_rdb, mpc85xxrdb_publish_pci_device);
machine_arch_initcall(p1020_rdb, mpc85xxrdb_publish_pci_device);
machine_arch_initcall(p1020_rdb_pc, mpc85xxrdb_publish_pci_device);
machine_arch_initcall(p1020_mbg_pc, mpc85xxrdb_publish_pci_device);
machine_arch_initcall(p1020_utm_pc, mpc85xxrdb_publish_pci_device);
machine_arch_initcall(p2020_rdb_pc, mpc85xxrdb_publish_pci_device);
machine_arch_initcall(p1024_rdb, mpc85xxrdb_publish_pci_device);
machine_arch_initcall(p1021_rdb_pc, mpc85xxrdb_publish_pci_device);
machine_arch_initcall(p1025_rdb, mpc85xxrdb_publish_pci_device);

static struct of_device_id __initdata mpc85xxrdb_ids[] = {
	{ .type = "soc", },
	{ .compatible = "soc", },
	{ .compatible = "simple-bus", },
	{ .type = "qe", },
	{ .compatible = "fsl,qe", },
	{ .compatible = "gianfar", },
	{},
};

static int __init mpc85xxrdb_publish_devices(void)
{
	return of_platform_bus_probe(NULL, mpc85xxrdb_ids, NULL);
}

machine_device_initcall(p2020_rdb, mpc85xxrdb_publish_devices);
machine_device_initcall(p2020_rdb_pc, mpc85xxrdb_publish_devices);
machine_device_initcall(p1020_rdb, mpc85xxrdb_publish_devices);
machine_device_initcall(p1020_rdb_pc, mpc85xxrdb_publish_devices);
machine_device_initcall(p1020_utm_pc, mpc85xxrdb_publish_devices);
machine_device_initcall(p1020_mbg_pc, mpc85xxrdb_publish_devices);
machine_device_initcall(p1021_rdb_pc, mpc85xxrdb_publish_devices);
machine_device_initcall(p1024_rdb, mpc85xxrdb_publish_devices);
machine_device_initcall(p1025_rdb, mpc85xxrdb_publish_devices);

/*
 * Called very early, device-tree isn't unflattened
 */
static int __init p2020_rdb_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	if (of_flat_dt_is_compatible(root, "fsl,P2020RDB"))
		return 1;
	return 0;
}

static int __init p1020_rdb_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	if (of_flat_dt_is_compatible(root, "fsl,P1020RDB"))
		return 1;
	return 0;
}

static int __init p1020_rdb_pc_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	return of_flat_dt_is_compatible(root, "fsl,P1020RDB-PC");
}

static int __init p1020_mbg_pc_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	return of_flat_dt_is_compatible(root, "fsl,P1020MBG-PC");
}

static int __init p1020_utm_pc_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	return of_flat_dt_is_compatible(root, "fsl,P1020UTM-PC");
}

static int __init p1021_rdb_pc_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	if (of_flat_dt_is_compatible(root, "fsl,P1021RDB-PC"))
		return 1;
	return 0;
}

static int __init p1024_rdb_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	return of_flat_dt_is_compatible(root, "fsl,P1024RDB");
}

static int __init p1025_rdb_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	return of_flat_dt_is_compatible(root, "fsl,P1025RDB");
}

static int __init p2020_rdb_pc_probe(void)
{
	unsigned long root = of_get_flat_dt_root();

	if (of_flat_dt_is_compatible(root, "fsl,P2020RDB-PC"))
		return 1;
	return 0;
}

define_machine(p2020_rdb) {
	.name			= "P2020 RDB",
	.probe			= p2020_rdb_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};

define_machine(p1020_rdb) {
	.name			= "P1020 RDB",
	.probe			= p1020_rdb_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};

define_machine(p1020_rdb_pc) {
	.name			= "P1020RDB-PC",
	.probe			= p1020_rdb_pc_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};

define_machine(p1020_mbg_pc) {
	.name			= "P1020 MBG-PC",
	.probe			= p1020_mbg_pc_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};

define_machine(p1020_utm_pc) {
	.name			= "P1020 UTM-PC",
	.probe			= p1020_utm_pc_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};

define_machine(p1021_rdb_pc) {
	.name			= "P1021 RDB-PC",
	.probe			= p1021_rdb_pc_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};

define_machine(p1024_rdb) {
	.name			= "P1024 RDB",
	.probe			= p1024_rdb_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};

define_machine(p1025_rdb) {
	.name			= "P1025 RDB",
	.probe			= p1025_rdb_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};

define_machine(p2020_rdb_pc) {
	.name			= "P2020RDB-PC",
	.probe			= p2020_rdb_pc_probe,
	.setup_arch		= mpc85xx_rdb_setup_arch,
	.init_IRQ		= mpc85xx_rdb_pic_init,
#ifdef CONFIG_PCI
	.pcibios_fixup_bus	= fsl_pcibios_fixup_bus,
#endif
	.get_irq		= mpic_get_irq,
	.restart		= fsl_rstcr_restart,
	.calibrate_decr		= generic_calibrate_decr,
	.progress		= udbg_progress,
};
