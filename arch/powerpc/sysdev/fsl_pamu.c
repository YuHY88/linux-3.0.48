/* Copyright (c) 2008-2012 Freescale Semiconductor, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Freescale Semiconductor nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *
 * ALTERNATIVELY, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") as published by the Free Software
 * Foundation, either version 2 of that License or (at your option) any
 * later version.
 *
 * THIS SOFTWARE IS PROVIDED BY Freescale Semiconductor "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Freescale Semiconductor BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/of_platform.h>
#include <linux/io.h>
#include <asm/fsl_guts.h>
#include <asm/fsl_hcalls.h>

/* PAMU CCSR space */
#define PAMU_PGC 0x00000000     /* Allows all peripheral accesses */
#define PAMU_PE 0x40000000      /* enable PAMU                    */

/* PAMU_OFFSET to the next pamu space in ccsr */
#define PAMU_OFFSET 0x1000

#define PAMU_MMAP_REGS_BASE 0

struct pamu_mmap_regs {
	u32 ppbah;
	u32 ppbal;
	u32 pplah;
	u32 pplal;
	u32 spbah;
	u32 spbal;
	u32 splah;
	u32 splal;
	u32 obah;
	u32 obal;
	u32 olah;
	u32 olal;
};

/* PAMU Error Registers */
#define PAMU_POES1	0x0040
#define PAMU_POES2	0x0044
#define PAMU_POEAH	0x0048
#define PAMU_POEAL	0x004C
#define PAMU_AVS1	0x0050
#define PAMU_AVS1_AV	0x1
#define PAMU_AVS1_OTV	0x6
#define PAMU_AVS1_APV	0x78
#define PAMU_AVS1_WAV	0x380
#define PAMU_AVS1_LAV	0x1c00
#define PAMU_AVS1_GCV	0x2000
#define PAMU_AVS1_PDV	0x4000
#define PAMU_AV_MASK	(PAMU_AVS1_AV | PAMU_AVS1_OTV | PAMU_AVS1_APV | \
			 PAMU_AVS1_WAV | PAMU_AVS1_LAV | PAMU_AVS1_GCV | \
			 PAMU_AVS1_PDV)
#define PAMU_AVS1_LIODN_SHIFT 16
#define PAMU_LAV_LIODN_NOT_IN_PPAACT 0x400

#define PAMU_AVS2	0x0054
#define PAMU_AVAH	0x0058
#define PAMU_AVAL	0x005C
#define PAMU_EECTL	0x0060
#define PAMU_EEDIS	0x0064
#define PAMU_EEINTEN	0x0068
#define PAMU_EEDET	0x006C
#define PAMU_EEATTR	0x0070
#define PAMU_EEAHI	0x0074
#define PAMU_EEALO	0x0078
#define PAMU_EEDHI	0X007C
#define PAMU_EEDLO	0x0080
#define PAMU_EECC	0x0084
#define PAMU_UDAD	0x0090

/* PAMU Revision Registers */
#define PAMU_PR1	0x0BF8
#define PAMU_PR2	0x0BFC

/* PAMU Capabilities Registers */
#define PAMU_PC1	0x0C00
#define PAMU_PC2	0x0C04
#define PAMU_PC3	0x0C08
#define PAMU_PC4	0x0C0C

/* PAMU Control Register */
#define PAMU_PC		0x0C10

/* PAMU control defs */
#define PAMU_CONTROL	0x0C10
#define PAMU_PC_PGC	0x80000000 /* 1 = PAMU Gate Closed : block all
peripheral access, 0 : may allow peripheral access */

#define PAMU_PC_PE	0x40000000 /* 0 = PAMU disabled, 1 = PAMU enabled */
#define PAMU_PC_SPCC	0x00000010 /* sPAACE cache enable */
#define PAMU_PC_PPCC	0x00000001 /* pPAACE cache enable */
#define PAMU_PC_OCE	0x00001000 /* OMT cache enable */

#define PAMU_PFA1	0x0C14
#define PAMU_PFA2	0x0C18

/* PAMU Interrupt control and Status Register */
#define PAMU_PICS			0x0C1C
#define PAMU_ACCESS_VIOLATION_STAT	0x8
#define PAMU_ACCESS_VIOLATION_ENABLE	0x4

/* PAMU Debug Registers */
#define PAMU_PD1	0x0F00
#define PAMU_PD2	0x0F04
#define PAMU_PD3	0x0F08
#define PAMU_PD4	0x0F0C

#define PAACE_AP_PERMS_DENIED	0x0
#define PAACE_AP_PERMS_QUERY	0x1
#define PAACE_AP_PERMS_UPDATE	0x2
#define PAACE_AP_PERMS_ALL	0x3
#define PAACE_DD_TO_HOST	0x0
#define PAACE_DD_TO_IO		0x1
#define PAACE_PT_PRIMARY	0x0
#define PAACE_PT_SECONDARY	0x1
#define PAACE_V_INVALID		0x0
#define PAACE_V_VALID		0x1
#define PAACE_MW_SUBWINDOWS	0x1

#define PAACE_WSE_4K		0xB
#define PAACE_WSE_8K		0xC
#define PAACE_WSE_16K		0xD
#define PAACE_WSE_32K		0xE
#define PAACE_WSE_64K		0xF
#define PAACE_WSE_128K		0x10
#define PAACE_WSE_256K		0x11
#define PAACE_WSE_512K		0x12
#define PAACE_WSE_1M		0x13
#define PAACE_WSE_2M		0x14
#define PAACE_WSE_4M		0x15
#define PAACE_WSE_8M		0x16
#define PAACE_WSE_16M		0x17
#define PAACE_WSE_32M		0x18
#define PAACE_WSE_64M		0x19
#define PAACE_WSE_128M		0x1A
#define PAACE_WSE_256M		0x1B
#define PAACE_WSE_512M		0x1C
#define PAACE_WSE_1G		0x1D
#define PAACE_WSE_2G		0x1E
#define PAACE_WSE_4G		0x1F

#define PAACE_DID_PCI_EXPRESS_1	0x00
#define PAACE_DID_PCI_EXPRESS_2	0x01
#define PAACE_DID_PCI_EXPRESS_3	0x02
#define PAACE_DID_PCI_EXPRESS_4	0x03
#define PAACE_DID_LOCAL_BUS	0x04
#define PAACE_DID_SRIO		0x0C
#define PAACE_DID_MEM_1		0x10
#define PAACE_DID_MEM_2		0x11
#define PAACE_DID_MEM_3		0x12
#define PAACE_DID_MEM_4		0x13
#define PAACE_DID_MEM_1_2	0x14
#define PAACE_DID_MEM_3_4	0x15
#define PAACE_DID_MEM_1_4	0x16
#define PAACE_DID_BM_SW_PORTAL	0x18
#define PAACE_DID_PAMU		0x1C
#define PAACE_DID_CAAM		0x21
#define PAACE_DID_QM_SW_PORTAL	0x3C
#define PAACE_DID_CORE0_INST	0x80
#define PAACE_DID_CORE0_DATA	0x81
#define PAACE_DID_CORE1_INST	0x82
#define PAACE_DID_CORE1_DATA	0x83
#define PAACE_DID_CORE2_INST	0x84
#define PAACE_DID_CORE2_DATA	0x85
#define PAACE_DID_CORE3_INST	0x86
#define PAACE_DID_CORE3_DATA	0x87
#define PAACE_DID_CORE4_INST	0x88
#define PAACE_DID_CORE4_DATA	0x89
#define PAACE_DID_CORE5_INST	0x8A
#define PAACE_DID_CORE5_DATA	0x8B
#define PAACE_DID_CORE6_INST	0x8C
#define PAACE_DID_CORE6_DATA	0x8D
#define PAACE_DID_CORE7_INST	0x8E
#define PAACE_DID_CORE7_DATA	0x8F
#define PAACE_DID_BROADCAST	0xFF

#define PAACE_ATM_NO_XLATE	0x00
#define PAACE_ATM_WINDOW_XLATE	0x01
#define PAACE_ATM_PAGE_XLATE	0x02
#define PAACE_ATM_WIN_PG_XLATE	(PAACE_ATM_WINDOW_XLATE | PAACE_ATM_PAGE_XLATE)
#define PAACE_OTM_NO_XLATE	0x00
#define PAACE_OTM_IMMEDIATE	0x01
#define PAACE_OTM_INDEXED	0x02
#define PAACE_OTM_RESERVED	0x03

#define PAACE_M_COHERENCE_REQ	0x01

#define PAACE_TCEF_FORMAT0_8B	0x00
#define PAACE_TCEF_FORMAT1_RSVD	0x01

#define PAACE_NUMBER_ENTRIES	0xFF

#define OME_NUMBER_ENTRIES	16   /* based on P4080 2.0 silicon plan */

/* PAMU Data Structures */

struct ppaace {
	/* PAACE Offset 0x00 */
	/* Window Base Address */
	u32		wbah;
	unsigned int	wbal:20;
	/* Window Size, 2^(N+1), N must be > 10 */
	unsigned int	wse:6;
	/* 1 Means there are secondary windows, wce is count */
	unsigned int	mw:1;
	/* Permissions, see PAACE_AP_PERMS_* defines */
	unsigned int	ap:2;
	/*
	 * Destination Domain, see PAACE_DD_* defines,
	 * defines data structure reference for ingress ops into
	 * host/coherency domain or ingress ops into I/O domain
	 */
	unsigned int	dd:1;
	/* PAACE Type, see PAACE_PT_* defines */
	unsigned int	pt:1;
	/* PAACE Valid, 0 is invalid */
	unsigned int	v:1;

	/* PAACE Offset 0x08 */
	/* Interpretation of first 32 bits dependent on DD above */
	union {
		struct {
			/* Destination ID, see PAACE_DID_* defines */
			u8 did;
			/* Partition ID */
			u8 pid;
			/* Snoop ID */
			u8 snpid;
			unsigned int coherency_required:1;
			unsigned int reserved:7;
		} to_host;
		struct {
			/* Destination ID, see PAACE_DID_* defines */
			u8 did;
			unsigned int __reserved:24;
		} to_io;
	} __packed domain_attr;
	/* Implementation attributes */
	struct {
		unsigned int reserved1:8;
		unsigned int cid:8;
		unsigned int reserved2:8;
	} __packed impl_attr;
	/* Window Count; 2^(N+1) sub-windows; only valid for primary PAACE */
	unsigned int wce:4;
	/* Address translation mode, see PAACE_ATM_* defines */
	unsigned int atm:2;
	/* Operation translation mode, see PAACE_OTM_* defines */
	unsigned int otm:2;

	/* PAACE Offset 0x10 */
	/* Translated window base address */
	u32 twbah;
	unsigned int twbal:20;
	/* Subwindow size encoding; 2^(N+1), N > 10 */
	unsigned int swse:6;
	unsigned int reserved4:6;

	/* PAACE Offset 0x18 */
	u32 fspi;
	union {
		struct {
			u8 ioea;
			u8 moea;
			u8 ioeb;
			u8 moeb;
		} immed_ot;
		struct {
			u16 reserved;
			u16 omi;
		} index_ot;
	} __packed op_encode;

	/* PAACE Offset 0x20 */
	u32 sbah;
	unsigned int sbal:20;
	unsigned int sse:6;
	unsigned int reserved5:6;

	/* PAACE Offset 0x28 */
	u32 tctbah;
	unsigned int tctbal:20;
	unsigned int pse:6;
	unsigned int tcef:1;
	unsigned int reserved6:5;

	/* PAACE Offset 0x30 */
	u32 reserved7[2];

	/* PAACE Offset 0x38 */
	u32 reserved8[2];
} __packed ppaace;

/* MOE : Mapped Operation Encodings */
#define NUM_MOE 128
struct ome {
	u8 moe[NUM_MOE];
} __packed ome;

/*
 * The Primary Peripheral Access Authorization and Control Table
 *
 * To keep things simple, we use one shared PPAACT for all PAMUs.  This means
 * that LIODNs must be unique across all PAMUs.
 */
static struct ppaace *ppaact;
static phys_addr_t ppaact_phys;

/* TRUE if we're running under the Freescale hypervisor */
bool has_fsl_hypervisor;

#define PAACT_SIZE              (sizeof(struct ppaace) * PAACE_NUMBER_ENTRIES)
#define OMT_SIZE                (sizeof(struct ome) * OME_NUMBER_ENTRIES)

#define IOE_READ        0x00
#define IOE_READ_IDX    0x00
#define IOE_WRITE       0x81
#define IOE_WRITE_IDX   0x01
#define IOE_EREAD0      0x82    /* Enhanced read type 0 */
#define IOE_EREAD0_IDX  0x02    /* Enhanced read type 0 */
#define IOE_EWRITE0     0x83    /* Enhanced write type 0 */
#define IOE_EWRITE0_IDX 0x03    /* Enhanced write type 0 */
#define IOE_DIRECT0     0x84    /* Directive type 0 */
#define IOE_DIRECT0_IDX 0x04    /* Directive type 0 */
#define IOE_EREAD1      0x85    /* Enhanced read type 1 */
#define IOE_EREAD1_IDX  0x05    /* Enhanced read type 1 */
#define IOE_EWRITE1     0x86    /* Enhanced write type 1 */
#define IOE_EWRITE1_IDX 0x06    /* Enhanced write type 1 */
#define IOE_DIRECT1     0x87    /* Directive type 1 */
#define IOE_DIRECT1_IDX 0x07    /* Directive type 1 */
#define IOE_RAC         0x8c    /* Read with Atomic clear */
#define IOE_RAC_IDX     0x0c    /* Read with Atomic clear */
#define IOE_RAS         0x8d    /* Read with Atomic set */
#define IOE_RAS_IDX     0x0d    /* Read with Atomic set */
#define IOE_RAD         0x8e    /* Read with Atomic decrement */
#define IOE_RAD_IDX     0x0e    /* Read with Atomic decrement */
#define IOE_RAI         0x8f    /* Read with Atomic increment */
#define IOE_RAI_IDX     0x0f    /* Read with Atomic increment */

#define EOE_READ        0x00
#define EOE_WRITE       0x01
#define EOE_RAC         0x0c    /* Read with Atomic clear */
#define EOE_RAS         0x0d    /* Read with Atomic set */
#define EOE_RAD         0x0e    /* Read with Atomic decrement */
#define EOE_RAI         0x0f    /* Read with Atomic increment */
#define EOE_LDEC        0x10    /* Load external cache */
#define EOE_LDECL       0x11    /* Load external cache with stash lock */
#define EOE_LDECPE      0x12    /* Load ext. cache with preferred exclusive */
#define EOE_LDECPEL     0x13    /* Load ext. cache w/ preferred excl. & lock */
#define EOE_LDECFE      0x14    /* Load external cache with forced exclusive */
#define EOE_LDECFEL     0x15    /* Load ext. cache w/ forced excl. & lock */
#define EOE_RSA         0x16    /* Read with stash allocate */
#define EOE_RSAU        0x17    /* Read with stash allocate and unlock */
#define EOE_READI       0x18    /* Read with invalidate */
#define EOE_RWNITC      0x19    /* Read with no intention to cache */
#define EOE_WCI         0x1a    /* Write cache inhibited */
#define EOE_WWSA        0x1b    /* Write with stash allocate */
#define EOE_WWSAL       0x1c    /* Write with stash allocate and lock */
#define EOE_WWSAO       0x1d    /* Write with stash allocate only */
#define EOE_WWSAOL      0x1e    /* Write with stash allocate only and lock */
#define EOE_VALID       0x80

/* define indexes for each operation mapping scenario */
#define OMI_QMAN        0x00
#define OMI_FMAN        0x01
#define OMI_QMAN_PRIV   0x02
#define OMI_CAAM        0x03

/*
 * Return the Nth integer of a given property in a given node
 *
 * 'index' is the index into the property (e.g. 'N').
 * 'property' is the name of the property.
 *
 * This function assumes the value of the property is <= INT_MAX.  A negative
 * return value indicates an error.
 */
static int of_read_indexed_number(struct device_node *node,
	const char *property, unsigned int index)
{
	const u32 *prop;
	int value;
	int len;

	prop = of_get_property(node, property, &len);
	if (!prop || (len % sizeof(uint32_t)))
		return -ENODEV;

	if (index >= (len / sizeof(uint32_t)))
		return -EINVAL;

	value = be32_to_cpu(prop[index]);

	return value;
}

/**
 * pamu_set_stash_dest() - set the stash target for a given LIODN
 * @liodn: LIODN to set
 * @cache_level: target cache level (1, 2, or 3)
 * @cpu: target CPU (0, 1, 2, etc)
 *
 * This function sets the stash target for a given LIODN, assuming that the
 * PAACE entry for that LIODN is already configured.
 *
 * The function returns 0 on success, or a negative error code on failure.
 */
int pamu_set_stash_dest(struct device_node *node, unsigned int index,
	unsigned int cpu, unsigned int cache_level)
{
	int liodn;
	const u32 *prop;
	unsigned int i;
#ifdef CONFIG_FSL_PAMU_ERRATUM_A_004510
	/*
	 * The work-around says that we cannot have multiple writes to the
	 * PAACT in flight simultaneously, which could happen if multiple
	 * cores try to update CID simultaneously.  To prevent that, we wrap
	 * the write in a mutex, which will force the cores to perform their
	 * updates in sequence.
	 */
	static DEFINE_SPINLOCK(pamu_lock);
#endif


	/* If we're running under a support hypervisor, make an hcall instead */
	if (has_fsl_hypervisor) {
		struct fh_dma_attr_stash attr;
		phys_addr_t paddr = virt_to_phys(&attr);
		int handle;

		handle = of_read_indexed_number(node, "fsl,hv-dma-handle",
						index);

		if (handle < 0)
			return -EINVAL;

		attr.vcpu = cpu;
		attr.cache = cache_level;

		if (fh_dma_attr_set(handle, FSL_PAMU_ATTR_STASH, paddr))
			return -EINVAL;

		return 0;
	}

	liodn = of_read_indexed_number(node, "fsl,liodn", index);
	if (liodn < 0)
		return liodn;

	for_each_node_by_type(node, "cpu") {
		prop = of_get_property(node, "reg", NULL);
		if (prop && (be32_to_cpup(prop) == cpu))
			goto found_cpu;
	}

	pr_err("fsl-pamu: could not find 'cpu' node %u\n", cpu);
	return -EINVAL;

found_cpu:
	/*
	 * Traverse the list of caches until we find the one we want.  The CPU
	 * node is also the L1 cache node
	 */
	for (i = 1; i < cache_level; i++) {
		node = of_parse_phandle(node, "next-level-cache", 0);
		if (!node) {
			pr_err("fsl-pamu: cache level %u invalid for cpu %u\n",
			       i, cpu);
			return -EINVAL;
		}
	}

	prop = of_get_property(node, "cache-stash-id", NULL);
	if (!prop) {
		pr_err("fsl-pamu: missing 'cache-stash-id' in %s\n",
		       node->full_name);
		return -EINVAL;
	}

#ifdef CONFIG_FSL_PAMU_ERRATUM_A_004510
	spin_lock(&pamu_lock);
#endif

	ppaact[liodn].impl_attr.cid = be32_to_cpup(prop);
	mb();

#ifdef CONFIG_FSL_PAMU_ERRATUM_A_004510
	spin_unlock(&pamu_lock);
#endif

	return 0;
}
EXPORT_SYMBOL(pamu_set_stash_dest);

/**
 * pamu_get_liodn_count() - returns the number of LIODNs for a given node
 * @node: the node to query
 *
 * This function returns the number of LIODNs in a given node.
 *
 * The function returns the number >= 0 on success, or a negative error code
 * on failure.  Currently, an error code cannot be returned, but that may
 * change in the future.  Callers are still expected to test for an error.
 */
int pamu_get_liodn_count(struct device_node *node)
{
	const u32 *prop;
	int len;

	/*
	 * Under the hypervisor, use the "fsl,hv-dma-handle".  Otherwise,
	 * use the "fsl,liodn" property.
	 */
	if (has_fsl_hypervisor)
		prop = of_get_property(node, "fsl,hv-dma-handle", &len);
	else
		prop = of_get_property(node, "fsl,liodn", &len);

	if (!prop)
		/*
		 * KVM sets up default stashing but does not provide an
		 * interface to the PAMU, so there are no PAMU nodes or LIODN
		 * properties in the guest device tree.  Therefore, if the
		 * LIODN property is missing, that doesn't mean that 'node' is
		 * invalid.
		 */
		return 0;

	return len / sizeof(uint32_t);
}
EXPORT_SYMBOL(pamu_get_liodn_count);


static void __init setup_omt(struct ome *omt)
{
	struct ome *ome;

	/* Configure OMI_QMAN */
	ome = &omt[OMI_QMAN];

	ome->moe[IOE_READ_IDX] = EOE_VALID | EOE_READ;
	ome->moe[IOE_EREAD0_IDX] = EOE_VALID | EOE_RSA;
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
	ome->moe[IOE_EWRITE0_IDX] = EOE_VALID | EOE_WWSAO;

	/*
	 * When it comes to stashing DIRECTIVEs, the QMan BG says
	 * (1.5.6.7.1:  FQD Context_A field used for dequeued etc.
	 * etc. stashing control):
	 * - AE/DE/CE == 0:  don't stash exclusive.  Use DIRECT0,
	 *                   which should be a non-PE LOADEC.
	 * - AE/DE/CE == 1:  stash exclusive via DIRECT1, i.e.
	 *                   LOADEC-PE
	 * If one desires to alter how the three different types of
	 * stashing are done, please alter rx_conf.exclusive in
	 * ipfwd_a.c (that specifies the 3-bit AE/DE/CE field), and
	 * do not alter the settings here.  - bgrayson
	 */
	ome->moe[IOE_DIRECT0_IDX] = EOE_VALID | EOE_LDEC;
	ome->moe[IOE_DIRECT1_IDX] = EOE_VALID | EOE_LDECPE;

	/* Configure OMI_FMAN */
	ome = &omt[OMI_FMAN];
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READI;
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;

	/* Configure OMI_QMAN private */
	ome = &omt[OMI_QMAN_PRIV];
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READ;
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
	ome->moe[IOE_EREAD0_IDX] = EOE_VALID | EOE_RSA;
	ome->moe[IOE_EWRITE0_IDX] = EOE_VALID | EOE_WWSA;

	/* Configure OMI_CAAM */
	ome = &omt[OMI_CAAM];
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READI;
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
}

static u32 __init get_stash_id(unsigned int stash_dest_hint,
	struct device_node *portal_dn)
{
	const u32 *prop;
	struct device_node *node;
	unsigned int cache_level;

	/* Fastpath, exit early if 3/CPC cache is target for stashing */
	if (stash_dest_hint == 3) {
		node = of_find_compatible_node(NULL, NULL,
				"fsl,p4080-l3-cache-controller");
		if (node) {
			prop = of_get_property(node, "cache-stash-id", 0);
			if (!prop) {
				pr_err("fsl-pamu: missing cache-stash-id in "
				       " %s\n", node->full_name);
				of_node_put(node);
				return ~(u32)0;
			}
			of_node_put(node);
			return *prop;
		}
		return ~(u32)0;
	}

	prop = of_get_property(portal_dn, "cpu-handle", 0);
	/* if no cpu-phandle assume that this is not a per-cpu portal */
	if (!prop)
		return ~(u32)0;

	node = of_find_node_by_phandle(*prop);
	if (!node) {
		pr_err("fsl-pamu: bad cpu-handle reference in %s\n",
				 portal_dn->full_name);
		return ~(u32)0;
	}

	/* find the hwnode that represents the cache */
	for (cache_level = 1; cache_level <= 3; cache_level++) {
		if (stash_dest_hint == cache_level) {
			prop = of_get_property(node, "cache-stash-id", 0);
			of_node_put(node);
			if (!prop) {
				pr_err("fsl-pamu: missing cache-stash-id in "
				       "%s\n", node->full_name);
				return ~(u32)0;
			}
			return *prop;
		}

		prop = of_get_property(node, "next-level-cache", 0);
		if (!prop) {
			pr_err("fsl-pamu: can't find next-level-cache in %s\n",
			       node->full_name);
			of_node_put(node);
			return ~(u32)0;  /* can't traverse any further */
		}
		of_node_put(node);

		/* advance to next node in cache hierarchy */
		node = of_find_node_by_phandle(*prop);
		if (!node) {
			pr_err("fsl-pamu: bad cpu phandle reference in %s\n",
			       portal_dn->full_name);
			return ~(u32)0;
		}
	}

	pr_err("fsl-pamu: stash destination not found for cache level %d "
	       "on portal node %s\n", stash_dest_hint, portal_dn->full_name);

	return ~(u32)0;
}

static void __init setup_liodns(void)
{
	int i, len;
	struct ppaace *ppaace;
	struct device_node *qman_portal_dn = NULL;
	struct device_node *qman_dn = NULL;
	struct device_node *bman_dn;
	const u32 *prop;
	u32 cache_id, prop_cnt;

	for (i = 0; i < PAACE_NUMBER_ENTRIES; i++) {
		ppaace = &ppaact[i];
		ppaace->pt = PAACE_PT_PRIMARY;
		ppaace->domain_attr.to_host.coherency_required =
				PAACE_M_COHERENCE_REQ;
		/* window size is 2^(WSE+1) bytes */
		ppaace->wse = 35; /* 36-bit phys. addr space */
		ppaace->wbah = ppaace->wbal = 0;
		ppaace->atm = PAACE_ATM_NO_XLATE;
		ppaace->ap = PAACE_AP_PERMS_ALL;
		mb();
		ppaace->v = 1;
	}

	/*
	 * Now, do specific stashing setup for qman portals.
	 * We need stashing setup for LIODNs for  qman portal(s) dqrr stashing
	 * (DLIODNs), qman portal(s) data stashing (FLIODNs)
	 */

	for_each_compatible_node(qman_portal_dn, NULL, "fsl,qman-portal") {
		pr_debug("qman portal %s found\n", qman_portal_dn->full_name);

		prop = of_get_property(qman_portal_dn, "fsl,liodn", &len);
		if (prop) {
			prop_cnt = len / sizeof(u32);
			do {
				pr_debug("liodn = %d\n", *prop);
				ppaace = &ppaact[*prop++];
				ppaace->otm = PAACE_OTM_INDEXED;
				ppaace->op_encode.index_ot.omi = OMI_QMAN;
				cache_id = get_stash_id(3, qman_portal_dn);
				pr_debug("cache_stash_id = %d\n", cache_id);
				if (~cache_id != 0)
					ppaace->impl_attr.cid = cache_id;
			} while (--prop_cnt);
		} else {
			pr_err("fsl-pamu: missing fsl,liodn property in %s\n",
			       qman_portal_dn->full_name);
		}
	}

	/*
	 * Next, do stashing setups for qman private memory access
	 */

	qman_dn = of_find_compatible_node(NULL, NULL, "fsl,qman");
	if (qman_dn) {
		prop = of_get_property(qman_dn, "fsl,liodn", NULL);
		if (prop) {
			ppaace = &ppaact[*prop];
			ppaace->otm = PAACE_OTM_INDEXED;
			ppaace->domain_attr.to_host.coherency_required = 0;
			ppaace->op_encode.index_ot.omi = OMI_QMAN_PRIV;
			cache_id = get_stash_id(3, qman_dn);
			pr_debug("cache_stash_id = %d\n", cache_id);
			if (~cache_id != 0)
				ppaace->impl_attr.cid = cache_id;
		} else {
			pr_err("fsl-pamu: missing fsl,liodn property in %s\n",
			       qman_dn->full_name);
		}
		of_node_put(qman_dn);
	}

	/*
	 * For liodn used by BMAN for its private memory accesses,
	 * turn the 'coherency required' off. This saves snoops to cores.
	 */

	bman_dn = of_find_compatible_node(NULL, NULL, "fsl,bman");
	if (bman_dn) {
		prop = of_get_property(bman_dn, "fsl,liodn", NULL);
		if (prop) {
			ppaace = &ppaact[*prop];
			ppaace->domain_attr.to_host.coherency_required = 0;
		} else {
			pr_err("fsl-pamu: missing fsl,liodn property in %s\n",
			       bman_dn->full_name);
		}
		of_node_put(bman_dn);
	}
}

static int __init setup_one_pamu(void *pamu_reg_base, struct ome *omt)
{
	struct pamu_mmap_regs *pamu_regs = pamu_reg_base + PAMU_MMAP_REGS_BASE;
	phys_addr_t phys;

	/* set up pointers to corenet control blocks */

	phys = ppaact_phys;
	out_be32(&pamu_regs->ppbah, upper_32_bits(phys));
	out_be32(&pamu_regs->ppbal, lower_32_bits(phys));

	phys = ppaact_phys + PAACE_NUMBER_ENTRIES * sizeof(struct ppaace);
	out_be32(&pamu_regs->pplah, upper_32_bits(phys));
	out_be32(&pamu_regs->pplal, lower_32_bits(phys));

	phys = virt_to_phys(omt);
	out_be32(&pamu_regs->obah, upper_32_bits(phys));
	out_be32(&pamu_regs->obal, lower_32_bits(phys));

	phys = virt_to_phys(omt + OME_NUMBER_ENTRIES);
	out_be32(&pamu_regs->olah, upper_32_bits(phys));
	out_be32(&pamu_regs->olal, lower_32_bits(phys));


	/*
	 * set PAMU enable bit,
	 * allow ppaact & omt to be cached
	 * & enable PAMU access violation interrupts.
	 */

	out_be32(pamu_reg_base + PAMU_PICS, PAMU_ACCESS_VIOLATION_ENABLE);
	out_be32(pamu_reg_base + PAMU_PC,
		 PAMU_PC_PE | PAMU_PC_OCE | PAMU_PC_SPCC | PAMU_PC_PPCC);

	return 0;
}

#define make64(high, low) (((u64)(high) << 32) | (low))

struct pamu_isr_data {
	void __iomem *pamu_reg_base;	/* Base address of PAMU regs*/
	unsigned int count;		/* The number of PAMUs */
};

static irqreturn_t pamu_av_isr(int irq, void *arg)
{
	struct pamu_isr_data *data = arg;
	phys_addr_t phys;
	unsigned int i, j;

	pr_emerg("fsl-pamu: access violation interrupt\n");

	for (i = 0; i < data->count; i++) {
		void __iomem *p = data->pamu_reg_base + i * PAMU_OFFSET;
		u32 pics = in_be32(p + PAMU_PICS);

		if (pics & PAMU_ACCESS_VIOLATION_STAT) {
			pr_emerg("POES1=%08x\n", in_be32(p + PAMU_POES1));
			pr_emerg("POES2=%08x\n", in_be32(p + PAMU_POES2));
			pr_emerg("AVS1=%08x\n", in_be32(p + PAMU_AVS1));
			pr_emerg("AVS2=%08x\n", in_be32(p + PAMU_AVS2));
			pr_emerg("AVA=%016llx\n", make64(in_be32(p + PAMU_AVAH),
				in_be32(p + PAMU_AVAL)));
			pr_emerg("UDAD=%08x\n", in_be32(p + PAMU_UDAD));
			pr_emerg("POEA=%016llx\n", make64(in_be32(p + PAMU_POEAH),
				in_be32(p + PAMU_POEAL)));

			phys = make64(in_be32(p + PAMU_POEAH),
				in_be32(p + PAMU_POEAL));

			/* Assume that POEA points to a PAACE */
			if (phys) {
				u32 *paace = phys_to_virt(phys);

				/* Only the first four words are relevant */
				for (j = 0; j < 4; j++)
					pr_emerg("PAACE[%u]=%08x\n", j, in_be32(paace + j));
			}
		}
	}

	panic("\n");

	/* NOT REACHED */
	return IRQ_HANDLED;
}

#ifdef CONFIG_FSL_PAMU_ERRATUM_A_004510

/*
 * The work-around for erratum A-004510 says we need to create a coherency
 * subdomain (CSD), which means we need to create a LAW (local access window)
 * just for the PAACT and OMT, and then give it a unique CSD ID.  Linux
 * normally doesn't touch the LAWs, so we define everything here.
 */

#define LAWAR_EN		0x80000000
#define LAWAR_TARGET_MASK	0x0FF00000
#define LAWAR_TARGET_SHIFT	20
#define LAWAR_SIZE_MASK		0x0000003F
#define LAWAR_CSDID_MASK	0x000FF000
#define LAWAR_CSDID_SHIFT	12

#define LAW_SIZE_4K		0xb

struct ccsr_law {
	u32	lawbarh;	/* LAWn base address high */
	u32	lawbarl;	/* LAWn base address low */
	u32	lawar;		/* LAWn attributes */
	u32	reserved;
};

/*
 * Create a coherence subdomain for a given memory block.
 */
static int __init create_csd(phys_addr_t phys, size_t size, u32 csd_port_id)
{
	struct device_node *np;
	const __be32 *iprop;
	void __iomem *lac = NULL;	/* Local Access Control registers */
	struct ccsr_law __iomem *law;
	void __iomem *ccm = NULL;
	u32 __iomem *csdids;
	unsigned int i, num_laws, num_csds;
	u32 law_target = 0;
	u32 csd_id = 0;
	int ret = 0;

	np = of_find_compatible_node(NULL, NULL, "fsl,corenet-law");
	if (!np)
		return -ENODEV;

	iprop = of_get_property(np, "fsl,num-laws", NULL);
	if (!iprop) {
		ret = -ENODEV;
		goto error;
	}

	num_laws = be32_to_cpup(iprop);
	if (!num_laws) {
		ret = -ENODEV;
		goto error;
	}

	lac = of_iomap(np, 0);
	if (!lac) {
		ret = -ENODEV;
		goto error;
	}

	/* LAW registers are at offset 0xC00 */
	law = lac + 0xC00;

	of_node_put(np);

	np = of_find_compatible_node(NULL, NULL, "fsl,corenet-cf");
	if (!np) {
		ret = -ENODEV;
		goto error;
	}

	iprop = of_get_property(np, "fsl,ccf-num-csdids", NULL);
	if (!iprop) {
		ret = -ENODEV;
		goto error;
	}

	num_csds = be32_to_cpup(iprop);
	if (!num_csds) {
		ret = -ENODEV;
		goto error;
	}

	ccm = of_iomap(np, 0);
	if (!ccm) {
		ret = -ENOMEM;
		goto error;
	}

	/* The undocumented CSDID registers are at offset 0x600 */
	csdids = ccm + 0x600;

	of_node_put(np);
	np = NULL;

	/* Find an unused coherence subdomain ID */
	for (csd_id = 0; csd_id < num_csds; csd_id++) {
		if (!csdids[csd_id])
			break;
	}

	/* Store the Port ID in the (undocumented) proper CIDMRxx register */
	csdids[csd_id] = csd_port_id;

	/* Find the DDR LAW that maps to our buffer. */
	for (i = 0; i < num_laws; i++) {
		if (law[i].lawar & LAWAR_EN) {
			phys_addr_t law_start, law_end;

			law_start = make64(law[i].lawbarh, law[i].lawbarl);
			law_end = law_start +
				(2ULL << (law[i].lawar & LAWAR_SIZE_MASK));

			if (law_start <= phys && phys < law_end) {
				law_target = law[i].lawar & LAWAR_TARGET_MASK;
				break;
			}
		}
	}

	if (i == 0 || i == num_laws) {
		/* This should never happen*/
		ret = -ENOENT;
		goto error;
	}

	/* Find a free LAW entry */
	while (law[--i].lawar & LAWAR_EN) {
		if (i == 0) {
			/* No higher priority LAW slots available */
			ret = -ENOENT;
			goto error;
		}
	}

	law[i].lawbarh = upper_32_bits(phys);
	law[i].lawbarl = lower_32_bits(phys);
	wmb();
	law[i].lawar = LAWAR_EN | law_target | (csd_id << LAWAR_CSDID_SHIFT) |
		(LAW_SIZE_4K + get_order(size));
	wmb();

error:
	if (ccm)
		iounmap(ccm);

	if (lac)
		iounmap(lac);

	if (np)
		of_node_put(np);

	return ret;
}
#endif

/*
 * Table of SVRs and the corresponding PORT_ID values.
 *
 * All future CoreNet-enabled SOCs will have this erratum fixed, so this table
 * should never need to be updated.  SVRs are guaranteed to be unique, so
 * there is no worry that a future SOC will inadvertently have one of these
 * values.
 */
static const struct {
	u32 svr;
	u32 port_id;
} port_id_map[] = {
	{0x82100010, 0xFF000000},	/* P2040 1.0 */
	{0x82100011, 0xFF000000},	/* P2040 1.1 */
	{0x82100110, 0xFF000000},	/* P2041 1.0 */
	{0x82100111, 0xFF000000},	/* P2041 1.1 */
	{0x82110310, 0xFF000000},	/* P3041 1.0 */
	{0x82110311, 0xFF000000},	/* P3041 1.1 */
	{0x82010020, 0xFFF80000},	/* P4040 2.0 */
	{0x82000020, 0xFFF80000},	/* P4080 2.0 */
	{0x82210010, 0xFC000000},       /* P5010 1.0 */
	{0x82210020, 0xFC000000},       /* P5010 2.0 */
	{0x82200010, 0xFC000000},	/* P5020 1.0 */
	{0x82050010, 0xFF800000},	/* P5021 1.0 */
	{0x82040010, 0xFF800000},	/* P5040 1.0 */
};

#define SVR_SECURITY	0x80000	/* The Security (E) bit */

static int __init fsl_pamu_probe(struct platform_device *pdev)
{
	void __iomem *pamu_regs = NULL;
	struct ccsr_guts __iomem *guts_regs = NULL;
	u32 pamubypenr, pamu_counter;
	unsigned long pamu_reg_off;
	struct device_node *guts_node;
	struct pamu_isr_data *data;
	u64 size;
	struct page *p;
	int ret = 0;
	struct ome *omt = NULL;
	int irq;
#ifdef CONFIG_FSL_PAMU_ERRATUM_A_004510
	size_t mem_size = 0;
	unsigned int order = 0;
	u32 csd_port_id = 0;
	unsigned i;
#endif

	/*
	 * enumerate all PAMUs and allocate and setup PAMU tables
	 * for each of them,
	 * NOTE : All PAMUs share the same LIODN tables.
	 */

	pamu_regs = of_iomap(pdev->dev.of_node, 0);
	if (!pamu_regs) {
		dev_err(&pdev->dev, "ioremap of PAMU node failed\n");
		return -ENOMEM;
	}
	of_get_address(pdev->dev.of_node, 0, &size, NULL);

	data = kzalloc(sizeof(struct pamu_isr_data), GFP_KERNEL);
	if (!data) {
		iounmap(pamu_regs);
		return -ENOMEM;
	}
	data->pamu_reg_base = pamu_regs;
	data->count = size / PAMU_OFFSET;

	irq = irq_of_parse_and_map(pdev->dev.of_node, 0);
	if (irq == NO_IRQ) {
		dev_warn(&pdev->dev, "no interrupts listed in PAMU node\n");
		goto error;
	}

	/* The ISR needs access to the regs, so we won't iounmap them */
	ret = request_irq(irq, pamu_av_isr, 0, "pamu", data);
	if (ret < 0) {
		dev_err(&pdev->dev, "error %i installing ISR for irq %i\n",
			ret, irq);
		goto error;
	}

	guts_node = of_find_compatible_node(NULL, NULL,
			"fsl,qoriq-device-config-1.0");
	if (!guts_node) {
		dev_err(&pdev->dev, "could not find GUTS node %s\n",
			pdev->dev.of_node->full_name);
		ret = -ENODEV;
		goto error;
	}

	guts_regs = of_iomap(guts_node, 0);
	of_node_put(guts_node);
	if (!guts_regs) {
		dev_err(&pdev->dev, "ioremap of GUTS node failed\n");
		ret = -ENODEV;
		goto error;
	}

#ifdef CONFIG_FSL_PAMU_ERRATUM_A_004510
	/*
	 * To simplify the allocation of a coherency domain, we allocate the
	 * PAACT and the OMT in the same memory buffer.  Unfortunately, this
	 * wastes more memory compared to allocating the buffers separately.
	 */

	/* Determine how much memory we need */
	mem_size = (PAGE_SIZE << get_order(PAACT_SIZE)) +
		(PAGE_SIZE << get_order(OMT_SIZE));
	order = get_order(mem_size);

	p = alloc_pages(GFP_KERNEL | __GFP_ZERO, order);
	if (!p) {
		dev_err(&pdev->dev, "unable to allocate PAACT/OMT block\n");
		ret = -ENOMEM;
		goto error;
	}

	ppaact = page_address(p);
	ppaact_phys = page_to_phys(p);

	/* Make sure the memory is naturally aligned */
	if (ppaact_phys & ((PAGE_SIZE << order) - 1)) {
		dev_err(&pdev->dev, "PAACT/OMT block is unaligned\n");
		ret = -ENOMEM;
		goto error;
	}

	/* This assumes that PAACT_SIZE is larger than OMT_SIZE */
	omt = (void *)ppaact + (PAGE_SIZE << get_order(PAACT_SIZE));

	dev_dbg(&pdev->dev, "ppaact virt=%p phys=0x%llx\n", ppaact,
		(unsigned long long) ppaact_phys);

	dev_dbg(&pdev->dev, "omt virt=%p phys=0x%llx\n", omt,
		(unsigned long long) virt_to_phys(omt));

	/* Check to see if we need to implement the work-around on this SOC */

	/* Determine the Port ID for our coherence subdomain */
	for (i = 0; i < ARRAY_SIZE(port_id_map); i++) {
		if (port_id_map[i].svr == (mfspr(SPRN_SVR) & ~SVR_SECURITY)) {
			csd_port_id = port_id_map[i].port_id;
			dev_dbg(&pdev->dev, "found matching SVR %08x\n",
				port_id_map[i].svr);
			break;
		}
	}

	if (csd_port_id) {
		dev_info(&pdev->dev, "implementing work-around for erratum "
			 "A-004510\n");
		dev_dbg(&pdev->dev, "creating coherency subdomain at address "
			"0x%llx, size %zu, port id 0x%08x", ppaact_phys,
			mem_size, csd_port_id);

		ret = create_csd(ppaact_phys, mem_size, csd_port_id);
		if (ret) {
			dev_err(&pdev->dev, "could not create coherence "
				"subdomain\n");
			return ret;
		}
	}
#else
	p = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order(PAACT_SIZE));
	if (!p) {
		dev_err(&pdev->dev, "unable to allocate PAACT table\n");
		ret = -ENOMEM;
		goto error;
	}
	ppaact = page_address(p);
	ppaact_phys = page_to_phys(p);

	dev_dbg(&pdev->dev, "ppaact virt=%p phys=0x%llx\n", ppaact,
		(unsigned long long) ppaact_phys);

	p = alloc_pages(GFP_KERNEL | __GFP_ZERO, get_order(OMT_SIZE));
	if (!p) {
		dev_err(&pdev->dev, "unable to allocate OMT table\n");
		ret = -ENOMEM;
		goto error;
	}
	omt = page_address(p);

	dev_dbg(&pdev->dev, "omt virt=%p phys=0x%llx\n", omt,
		(unsigned long long) page_to_phys(p));
#endif

	pamubypenr = in_be32(&guts_regs->pamubypenr);

	for (pamu_reg_off = 0, pamu_counter = 0x80000000; pamu_reg_off < size;
	     pamu_reg_off += PAMU_OFFSET, pamu_counter >>= 1) {
		setup_one_pamu(pamu_regs + pamu_reg_off, omt);

		/* Disable PAMU bypass for this PAMU */
		pamubypenr &= ~pamu_counter;
	}

	setup_omt(omt);

	/*
	 * setup all LIODNS(s) to define a 1:1 mapping for the entire
	 * 36-bit physical address space
	 */
	setup_liodns();
	mb();

	/* Enable all relevant PAMU(s) */
	out_be32(&guts_regs->pamubypenr, pamubypenr);

	iounmap(guts_regs);

	return 0;

error:
	if (irq != NO_IRQ)
		free_irq(irq, 0);

	if (pamu_regs)
		iounmap(pamu_regs);

	if (guts_regs)
		iounmap(guts_regs);

#ifdef CONFIG_FSL_PAMU_ERRATUM_A_004510
	if (ppaact)
		free_pages((unsigned long)ppaact, order);
#else
	if (ppaact)
		free_pages((unsigned long)ppaact, get_order(PAACT_SIZE));

	if (omt)
		free_pages((unsigned long)omt, get_order(OMT_SIZE));
#endif

	ppaact = NULL;
	ppaact_phys = 0;

	return ret;
}

static struct platform_driver fsl_of_pamu_driver = {
	.driver = {
		.name = "fsl-of-pamu",
		.owner = THIS_MODULE,
	},
	.probe = fsl_pamu_probe,
};

static bool is_fsl_hypervisor(void)
{
	struct device_node *np;
	struct property *prop;

	np = of_find_node_by_path("/hypervisor");
	if (!np)
		return false;

	prop = of_find_property(np, "fsl,has-stash-attr-hcall", NULL);
	of_node_put(np);

	if (!prop)
		pr_notice("fsl-pamu: this hypervisor does not support the "
			"stash attribute hypercall\n");

	return !!prop;
}

static __init int fsl_pamu_init(void)
{
	struct platform_device *pdev = NULL;
	struct device_node *np;
	int ret;

	/*
	 * The normal OF process calls the probe function at some
	 * indeterminate later time, after most drivers have loaded.  This is
	 * too late for us, because PAMU clients (like the Qman driver)
	 * depend on PAMU being initialized early.
	 *
	 * So instead, we "manually" call our probe function by creating the
	 * platform devices ourselves.
	 */

	/*
	 * We assume that there is only one PAMU node in the device tree.  A
	 * single PAMU node represents all of the PAMU devices in the SOC
	 * already.   Everything else already makes that assumption, and the
	 * binding for the PAMU nodes doesn't allow for any parent-child
	 * relationships anyway.  In other words, support for more than one
	 * PAMU node would require significant changes to a lot of code.
	 */

	np = of_find_compatible_node(NULL, NULL, "fsl,pamu");
	if (!np) {
		/* No PAMU nodes, so check for a hypervisor */
		if (is_fsl_hypervisor()) {
			has_fsl_hypervisor = true;
			/* Remain resident, but we don't need a platform */
			return 0;
		}

		pr_err("fsl-pamu: could not find a PAMU node\n");
		return -ENODEV;
	}

	ret = platform_driver_register(&fsl_of_pamu_driver);
	if (ret) {
		pr_err("fsl-pamu: could not register driver (err=%i)\n", ret);
		goto error_driver_register;
	}

	pdev = platform_device_alloc("fsl-of-pamu", 0);
	if (!pdev) {
		pr_err("fsl-pamu: could not allocate device %s\n",
		       np->full_name);
		ret = -ENOMEM;
		goto error_device_alloc;
	}
	pdev->dev.of_node = of_node_get(np);

	ret = platform_device_add(pdev);
	if (ret) {
		pr_err("fsl-pamu: could not add device %s (err=%i)\n",
		       np->full_name, ret);
		goto error_device_add;
	}

	return 0;

error_device_add:
	of_node_put(pdev->dev.of_node);
	pdev->dev.of_node = NULL;

	platform_device_put(pdev);

error_device_alloc:
	platform_driver_unregister(&fsl_of_pamu_driver);

error_driver_register:
	of_node_put(np);

	return ret;
}

arch_initcall(fsl_pamu_init);
