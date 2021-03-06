/*
 * drivers/net/gianfar.c
 *
 * Gianfar Ethernet Driver
 * This driver is designed for the non-CPM ethernet controllers
 * on the 85xx and 83xx family of integrated processors
 * Based on 8260_io/fcc_enet.c
 *
 * Author: Andy Fleming
 * Maintainer: Kumar Gala
 * Modifier: Sandeep Gopalpet <sandeep.kumar@freescale.com>
 *
 * Copyright 2002-2009, 2011-2012 Freescale Semiconductor, Inc.
 * Copyright 2007 MontaVista Software, Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 *  Gianfar:  AKA Lambda Draconis, "Dragon"
 *  RA 11 31 24.2
 *  Dec +69 19 52
 *  V 3.84
 *  B-V +1.62
 *
 *  Theory of operation
 *
 *  The driver is initialized through of_device. Configuration information
 *  is therefore conveyed through an OF-style device tree.
 *
 *  The Gianfar Ethernet Controller uses a ring of buffer
 *  descriptors.  The beginning is indicated by a register
 *  pointing to the physical address of the start of the ring.
 *  The end is determined by a "wrap" bit being set in the
 *  last descriptor of the ring.
 *
 *  When a packet is received, the RXF bit in the
 *  IEVENT register is set, triggering an interrupt when the
 *  corresponding bit in the IMASK register is also set (if
 *  interrupt coalescing is active, then the interrupt may not
 *  happen immediately, but will wait until either a set number
 *  of frames or amount of time have passed).  In NAPI, the
 *  interrupt handler will signal there is work to be done, and
 *  exit. This method will start at the last known empty
 *  descriptor, and process every subsequent descriptor until there
 *  are none left with data (NAPI will stop after a set number of
 *  packets to give time to other tasks, but will eventually
 *  process all the packets).  The data arrives inside a
 *  pre-allocated skb, and so after the skb is passed up to the
 *  stack, a new skb must be allocated, and the address field in
 *  the buffer descriptor must be updated to indicate this new
 *  skb.
 *
 *  When the kernel requests that a packet be transmitted, the
 *  driver starts where it left off last time, and points the
 *  descriptor at the buffer which was passed in.  The driver
 *  then informs the DMA engine that there are packets ready to
 *  be transmitted.  Once the controller is finished transmitting
 *  the packet, an interrupt may be triggered (under the same
 *  conditions as for reception, but depending on the TXF bit).
 *  The driver then cleans up the buffer.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#define DEBUG

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
#include <linux/if_vlan.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <linux/of_mdio.h>
#include <linux/of_platform.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <linux/inetdevice.h>
#include <sysdev/fsl_soc.h>
#include <linux/net_tstamp.h>

#include <asm/io.h>
#include <asm/reg.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/crc32.h>
#include <linux/mii.h>
#include <linux/phy.h>
#include <linux/phy_fixed.h>
#include <linux/of.h>
#include <linux/of_net.h>

#include "gianfar.h"
#include "fsl_pq_mdio.h"
#ifdef CONFIG_GIANFAR_L2SRAM
#include <asm/fsl_85xx_cache_sram.h>
#endif
#ifdef CONFIG_GFAR_HW_TCP_RECEIVE_OFFLOAD
#include <net/tcp.h>
#endif

#ifdef CONFIG_AS_FASTPATH
#include <linux/sched.h>
devfp_hook_t	devfp_rx_hook;
EXPORT_SYMBOL(devfp_rx_hook);

devfp_hook_t	devfp_tx_hook;
EXPORT_SYMBOL(devfp_tx_hook);

#endif
#ifdef CONFIG_RX_TX_BUFF_XCHG
#define RT_PKT_ID 0xff
#define KER_PKT_ID 0xfe
#define TX_TIMEOUT      (5*HZ)
#else
#define TX_TIMEOUT      (1*HZ)
#endif
#undef BRIEF_GFAR_ERRORS
#undef VERBOSE_GFAR_ERRORS

const char gfar_driver_name[] = "Gianfar Ethernet";
const char gfar_driver_version[] = "1.3";
static struct gfar_recycle_cntxt *gfar_global_recycle_cntxt;
static int tx_napi_enabled = 1;
static int tx_napi_weight = GFAR_DEV_TX_WEIGHT;
static int rx_napi_weight = GFAR_DEV_RX_WEIGHT;
module_param(tx_napi_enabled, bool, S_IRUGO);
module_param(tx_napi_weight, int, S_IRUGO);
module_param(rx_napi_weight, int, S_IRUGO);

MODULE_PARM_DESC(tx_napi_enabled, "Flag to control TX IRQ handling method: NAPI or No-NAPI(hw polling)");
MODULE_PARM_DESC(tx_napi_weight, "TX NAPI weight");
MODULE_PARM_DESC(rx_napi_weight, "RX NAPI weight");

static int gfar_enet_open(struct net_device *dev);
static int gfar_start_xmit(struct sk_buff *skb, struct net_device *dev);
static void gfar_reset_task(struct work_struct *work);
static void gfar_timeout(struct net_device *dev);
static int gfar_close(struct net_device *dev);
struct sk_buff *gfar_new_skb(struct net_device *dev);
void gfar_free_skb(struct sk_buff *skb);
static void gfar_new_rxbdp(struct gfar_priv_rx_q *rx_queue, struct rxbd8 *bdp,
		struct sk_buff *skb);
static int gfar_set_mac_address(struct net_device *dev);
static int gfar_change_mtu(struct net_device *dev, int new_mtu);
static irqreturn_t gfar_error(int irq, void *dev_id);
static irqreturn_t gfar_transmit(int irq, void *dev_id);
#ifdef CONFIG_RX_TX_BUFF_XCHG
static irqreturn_t gfar_enable_tx_queue(int irq, void *dev_id);
#else
static irqreturn_t gfar_transmit_no_napi(int irq, void *dev_id);
#endif
static irqreturn_t gfar_interrupt(int irq, void *dev_id);
static void adjust_link(struct net_device *dev);
static void init_registers(struct net_device *dev);
static int init_phy(struct net_device *dev);
static int gfar_probe(struct platform_device *ofdev);
static int gfar_remove(struct platform_device *ofdev);
static void free_skb_resources(struct gfar_private *priv);
static void gfar_set_multi(struct net_device *dev);
static void gfar_set_hash_for_addr(struct net_device *dev, u8 *addr);
static void gfar_configure_serdes(struct net_device *dev);
static int gfar_poll_rx(struct napi_struct *napi, int budget);
static int gfar_poll_tx(struct napi_struct *napi, int budget);
#ifdef CONFIG_NET_POLL_CONTROLLER
static void gfar_netpoll(struct net_device *dev);
#endif
static void gfar_schedule_rx_cleanup(struct gfar_priv_grp *gfargrp);
static void gfar_schedule_tx_cleanup(struct gfar_priv_grp *gfargrp);
int gfar_clean_rx_ring(struct gfar_priv_rx_q *rx_queue, int rx_work_limit);
static int gfar_clean_tx_ring(struct gfar_priv_tx_q *tx_queue,
		int tx_work_limit);

static int gfar_process_frame(struct net_device *dev, struct sk_buff *skb,
			      int amount_pull);
void gfar_halt(struct net_device *dev);
static void gfar_halt_nodisable(struct net_device *dev);
void gfar_start(struct net_device *dev);
static void gfar_clear_exact_match(struct net_device *dev);
static void gfar_set_mac_for_addr(struct net_device *dev, int num,
				  const u8 *addr);
static int gfar_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);

#ifdef CONFIG_PM
static void gfar_halt_rx(struct net_device *dev);
static void gfar_rx_start(struct net_device *dev);
static void gfar_enable_filer(struct net_device *dev);
static void gfar_disable_filer(struct net_device *dev);
static void gfar_config_filer_table(struct net_device *dev);
static void gfar_restore_filer_table(struct net_device *dev);
static int gfar_get_ip(struct net_device *dev);
#endif

MODULE_AUTHOR("Freescale Semiconductor, Inc");
MODULE_DESCRIPTION("Gianfar Ethernet Driver");
MODULE_LICENSE("GPL");

static void gfar_init_rxbdp(struct gfar_priv_rx_q *rx_queue, struct rxbd8 *bdp,
			    dma_addr_t buf)
{
	u32 lstatus;

	bdp->bufPtr = buf;

	lstatus = BD_LFLAG(RXBD_EMPTY | RXBD_INTERRUPT);
	if (bdp == rx_queue->rx_bd_base + rx_queue->rx_ring_size - 1)
		lstatus |= BD_LFLAG(RXBD_WRAP);

	eieio();

	bdp->lstatus = lstatus;
}

static int gfar_init_bds(struct net_device *ndev)
{
	struct gfar_private *priv = netdev_priv(ndev);
	struct gfar_priv_tx_q *tx_queue = NULL;
	struct gfar_priv_rx_q *rx_queue = NULL;
	struct txbd8 *txbdp;
	struct rxbd8 *rxbdp;
	int i, j;

	for (i = 0; i < priv->num_tx_queues; i++) {
		tx_queue = priv->tx_queue[i];
		/* Initialize some variables in our dev structure */
		tx_queue->num_txbdfree = tx_queue->tx_ring_size;
		tx_queue->dirty_tx = tx_queue->tx_bd_base;
		tx_queue->cur_tx = tx_queue->tx_bd_base;
		tx_queue->skb_curtx = 0;
		tx_queue->skb_dirtytx = 0;

		/* Initialize Transmit Descriptor Ring */
		txbdp = tx_queue->tx_bd_base;
		for (j = 0; j < tx_queue->tx_ring_size; j++) {
			txbdp->lstatus = 0;
			txbdp->bufPtr = 0;
			txbdp++;
		}

		/* Set the last descriptor in the ring to indicate wrap */
		txbdp--;
		txbdp->status |= TXBD_WRAP;
	}

	for (i = 0; i < priv->num_rx_queues; i++) {
		rx_queue = priv->rx_queue[i];
		rx_queue->cur_rx = rx_queue->rx_bd_base;
		rx_queue->skb_currx = 0;
		rxbdp = rx_queue->rx_bd_base;

		for (j = 0; j < rx_queue->rx_ring_size; j++) {
			struct sk_buff *skb = rx_queue->rx_skbuff[j];

			if (skb) {
				gfar_init_rxbdp(rx_queue, rxbdp,
						rxbdp->bufPtr);
			} else {
				skb = gfar_new_skb(ndev);
				if (!skb) {
					netdev_err(ndev, "Can't allocate RX buffers\n");
					goto err_rxalloc_fail;
				}
				rx_queue->rx_skbuff[j] = skb;

				gfar_new_rxbdp(rx_queue, rxbdp, skb);
			}

			rxbdp++;
		}

	}

	return 0;

err_rxalloc_fail:
	free_skb_resources(priv);
	return -ENOMEM;
}

static void *gfar_alloc_bds(struct gfar_private *priv, dma_addr_t *addr)
{
	void *vaddr = NULL;
#ifdef CONFIG_GIANFAR_L2SRAM
	phys_addr_t paddr;
	vaddr = mpc85xx_cache_sram_alloc(BDS_REGION_SIZE(priv), &paddr,
			L1_CACHE_BYTES);
	if (vaddr) {
		priv->l2sram_bds_en = 1;
		*addr = phys_to_dma(&priv->ofdev->dev, paddr);
		return vaddr;
	}
	/* fallback to normal memory rather than stop working */
#endif
	vaddr = dma_alloc_coherent(&priv->ofdev->dev,
			BDS_REGION_SIZE(priv), addr, GFP_KERNEL);

	return vaddr;
}

static int gfar_alloc_skb_resources(struct net_device *ndev)
{
	void *vaddr;
	dma_addr_t addr;
	int i, j, k;
	struct gfar_private *priv = netdev_priv(ndev);
	struct gfar_priv_tx_q *tx_queue = NULL;
	struct gfar_priv_rx_q *rx_queue = NULL;

	priv->total_tx_ring_size = 0;
	for (i = 0; i < priv->num_tx_queues; i++)
		priv->total_tx_ring_size += priv->tx_queue[i]->tx_ring_size;

	priv->total_rx_ring_size = 0;
	for (i = 0; i < priv->num_rx_queues; i++)
		priv->total_rx_ring_size += priv->rx_queue[i]->rx_ring_size;

	/* Allocate memory for the buffer descriptors */
	vaddr = gfar_alloc_bds(priv, &addr);
	if (!vaddr) {
		netif_err(priv, ifup, ndev,
			  "Could not allocate buffer descriptors!\n");
		return -ENOMEM;
	}

	for (i = 0; i < priv->num_tx_queues; i++) {
		tx_queue = priv->tx_queue[i];
		tx_queue->tx_bd_base = (struct txbd8 *) vaddr;
		tx_queue->tx_bd_dma_base = addr;
		tx_queue->dev = ndev;
		/* enet DMA only understands physical addresses */
		addr    += sizeof(struct txbd8) *tx_queue->tx_ring_size;
		vaddr   += sizeof(struct txbd8) *tx_queue->tx_ring_size;
	}

	/* Start the rx descriptor ring where the tx ring leaves off */
	for (i = 0; i < priv->num_rx_queues; i++) {
		rx_queue = priv->rx_queue[i];
		rx_queue->rx_bd_base = (struct rxbd8 *) vaddr;
		rx_queue->rx_bd_dma_base = addr;
		rx_queue->dev = ndev;
		addr    += sizeof (struct rxbd8) * rx_queue->rx_ring_size;
		vaddr   += sizeof (struct rxbd8) * rx_queue->rx_ring_size;
	}

	/* Setup the skbuff rings */
	for (i = 0; i < priv->num_tx_queues; i++) {
		tx_queue = priv->tx_queue[i];
		tx_queue->tx_skbuff = kmalloc(sizeof(*tx_queue->tx_skbuff) *
				  tx_queue->tx_ring_size, GFP_KERNEL);
		if (!tx_queue->tx_skbuff) {
			netif_err(priv, ifup, ndev,
				  "Could not allocate tx_skbuff\n");
			goto cleanup;
		}

		for (k = 0; k < tx_queue->tx_ring_size; k++)
			tx_queue->tx_skbuff[k] = NULL;
	}

	for (i = 0; i < priv->num_rx_queues; i++) {
		rx_queue = priv->rx_queue[i];
		rx_queue->rx_skbuff = kmalloc(sizeof(*rx_queue->rx_skbuff) *
				  rx_queue->rx_ring_size, GFP_KERNEL);

		if (!rx_queue->rx_skbuff) {
			netif_err(priv, ifup, ndev,
				  "Could not allocate rx_skbuff\n");
			goto cleanup;
		}

		for (j = 0; j < rx_queue->rx_ring_size; j++)
			rx_queue->rx_skbuff[j] = NULL;
	}

	if (gfar_init_bds(ndev))
		goto cleanup;

	return 0;

cleanup:
	free_skb_resources(priv);
	return -ENOMEM;
}

static void gfar_init_tx_rx_base(struct gfar_private *priv)
{
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 __iomem *baddr;
	int i;
	dma_addr_t addr;

	/*
	 * eTSEC supports 36-bit physical addressing.
	 * Should the BD rings be located at adresses above 4GB,
	 * initialize tbaseh/rbaseh with the upper 32 bits. This
	 * may happen when the BD rings are allocated in SRAM.
	 */
	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_36BIT_ADDR) {
		addr = priv->tx_queue[0]->tx_bd_dma_base;
		gfar_write(&regs->tbaseh, upper_32_bits(addr) & 0xf);
		addr = priv->rx_queue[0]->rx_bd_dma_base;
		gfar_write(&regs->rbaseh, upper_32_bits(addr) & 0xf);
	}

	baddr = &regs->tbase0;
	for(i = 0; i < priv->num_tx_queues; i++) {
		gfar_write(baddr, priv->tx_queue[i]->tx_bd_dma_base);
		baddr	+= 2;
	}

	baddr = &regs->rbase0;
	for(i = 0; i < priv->num_rx_queues; i++) {
		gfar_write(baddr, priv->rx_queue[i]->rx_bd_dma_base);
		baddr   += 2;
	}
}

static void gfar_init_mac(struct net_device *ndev)
{
	struct gfar_private *priv = netdev_priv(ndev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 rctrl = 0;
	u32 tctrl = 0;
	u32 attrs = 0;

	/* write the tx/rx base registers */
	gfar_init_tx_rx_base(priv);

	/* Configure the coalescing support */
	gfar_configure_tx_coalescing(priv, 0xFF);
	gfar_configure_rx_coalescing(priv, 0xFF);
	if (priv->rx_filer_enable) {
		rctrl |= RCTRL_FILREN;
		/* Program the RIR0 reg with the required distribution */
#ifdef CONFIG_GFAR_HW_TCP_RECEIVE_OFFLOAD
		if (priv->hw_tcp.en)
			gfar_write(&regs->rir0, TWO_QUEUE_RIR0);
		else
#endif
		gfar_write(&regs->rir0, DEFAULT_RIR0);
	}

	if (ndev->features & NETIF_F_RXCSUM)
		rctrl |= RCTRL_CHECKSUMMING;

	if (priv->extended_hash) {
		rctrl |= RCTRL_EXTHASH;

		gfar_clear_exact_match(ndev);
		rctrl |= RCTRL_EMEN;
	}

	if (priv->padding) {
		rctrl &= ~RCTRL_PAL_MASK;
		rctrl |= RCTRL_PADDING(priv->padding);
	}

	/* Insert receive time stamps into padding alignment bytes */
	if (priv->ptimer) {
		rctrl &= ~RCTRL_PAL_MASK;
		rctrl |= RCTRL_PADDING(8) | RCTRL_PRSDEP_INIT;
		priv->padding = 8;
	}

	/* Enable HW time stamping if requested from user space */
	if (priv->hwts_rx_en)
		rctrl |= RCTRL_PRSDEP_INIT | RCTRL_TS_ENABLE;

	if (ndev->features & NETIF_F_HW_VLAN_RX)
		rctrl |= RCTRL_VLEX | RCTRL_PRSDEP_INIT;

	/* Init rctrl based on our settings */
	gfar_write(&regs->rctrl, rctrl);

	if (ndev->features & NETIF_F_IP_CSUM)
		tctrl |= TCTRL_INIT_CSUM;

	if (priv->prio_sched_en)
		tctrl |= TCTRL_TXSCHED_PRIO;
	else {
		tctrl |= TCTRL_TXSCHED_WRRS;
		gfar_write(&regs->tr03wt, DEFAULT_WRRS_WEIGHT);
		gfar_write(&regs->tr47wt, DEFAULT_WRRS_WEIGHT);
	}

	gfar_write(&regs->tctrl, tctrl);

	/* Set the extraction length and index */
	attrs = ATTRELI_EL(priv->rx_stash_size) |
		ATTRELI_EI(priv->rx_stash_index);

	gfar_write(&regs->attreli, attrs);

	/* Start with defaults, and add stashing or locking
	 * depending on the approprate variables */
	attrs = ATTR_INIT_SETTINGS;

	if (priv->bd_stash_en)
		attrs |= ATTR_BDSTASH;

	if (priv->rx_stash_size != 0)
		attrs |= ATTR_BUFSTASH;

	gfar_write(&regs->attr, attrs);

	gfar_write(&regs->fifo_tx_thr, priv->fifo_threshold);
	gfar_write(&regs->fifo_tx_starve, priv->fifo_starve);
	gfar_write(&regs->fifo_tx_starve_shutoff, priv->fifo_starve_off);
}

static struct net_device_stats *gfar_get_stats(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	unsigned long rx_packets = 0, rx_bytes = 0, rx_dropped = 0;
	unsigned long tx_packets = 0, tx_bytes = 0;
	int i = 0;

	for (i = 0; i < priv->num_rx_queues; i++) {
		rx_packets += priv->rx_queue[i]->stats.rx_packets;
		rx_bytes += priv->rx_queue[i]->stats.rx_bytes;
		rx_dropped += priv->rx_queue[i]->stats.rx_dropped;
	}

	dev->stats.rx_packets = rx_packets;
	dev->stats.rx_bytes = rx_bytes;
	dev->stats.rx_dropped = rx_dropped;

	for (i = 0; i < priv->num_tx_queues; i++) {
		tx_bytes += priv->tx_queue[i]->stats.tx_bytes;
		tx_packets += priv->tx_queue[i]->stats.tx_packets;
	}

	dev->stats.tx_bytes = tx_bytes;
	dev->stats.tx_packets = tx_packets;

	return &dev->stats;
}

static const struct net_device_ops gfar_netdev_ops = {
	.ndo_open = gfar_enet_open,
	.ndo_start_xmit = gfar_start_xmit,
	.ndo_stop = gfar_close,
	.ndo_change_mtu = gfar_change_mtu,
	.ndo_set_features = gfar_set_features,
	.ndo_set_multicast_list = gfar_set_multi,
	.ndo_tx_timeout = gfar_timeout,
	.ndo_do_ioctl = gfar_ioctl,
	.ndo_get_stats = gfar_get_stats,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = gfar_netpoll,
#endif
};

void lock_rx_qs(struct gfar_private *priv)
{
	int i = 0x0;

	for (i = 0; i < priv->num_rx_queues; i++)
		spin_lock(&priv->rx_queue[i]->rxlock);
}

void lock_tx_qs(struct gfar_private *priv)
{
	int i = 0x0;

	for (i = 0; i < priv->num_tx_queues; i++)
		spin_lock(&priv->tx_queue[i]->txlock);
}

void unlock_rx_qs(struct gfar_private *priv)
{
	int i = 0x0;

	for (i = 0; i < priv->num_rx_queues; i++)
		spin_unlock(&priv->rx_queue[i]->rxlock);
}

void unlock_tx_qs(struct gfar_private *priv)
{
	int i = 0x0;

	for (i = 0; i < priv->num_tx_queues; i++)
		spin_unlock(&priv->tx_queue[i]->txlock);
}

static bool gfar_is_vlan_on(struct gfar_private *priv)
{
	return (priv->ndev->features & NETIF_F_HW_VLAN_RX) ||
	       (priv->ndev->features & NETIF_F_HW_VLAN_TX);
}

/* Returns 1 if incoming frames use an FCB */
static inline int gfar_uses_fcb(struct gfar_private *priv)
{
	return gfar_is_vlan_on(priv) ||
		(priv->ndev->features & NETIF_F_RXCSUM) ||
		(priv->device_flags & FSL_GIANFAR_DEV_HAS_TIMER);
}

static void free_tx_pointers(struct gfar_private *priv)
{
	int i = 0;

	for (i = 0; i < priv->num_tx_queues; i++)
		kfree(priv->tx_queue[i]);
}

static void free_rx_pointers(struct gfar_private *priv)
{
	int i = 0;

	for (i = 0; i < priv->num_rx_queues; i++)
		kfree(priv->rx_queue[i]);
}

static void unmap_group_regs(struct gfar_private *priv)
{
	int i = 0;

	for (i = 0; i < MAXGROUPS; i++)
		if (priv->gfargrp[i].regs)
			iounmap(priv->gfargrp[i].regs);
}

static void disable_napi(struct gfar_private *priv)
{
	int i = 0;

	for (i = 0; i < priv->num_grps; i++) {
		napi_disable(&priv->gfargrp[i].napi_rx);
		if (likely(tx_napi_enabled))
			napi_disable(&priv->gfargrp[i].napi_tx);
	}
}

static void enable_napi(struct gfar_private *priv)
{
	int i = 0;

	for (i = 0; i < priv->num_grps; i++) {
		napi_enable(&priv->gfargrp[i].napi_rx);
		if (likely(tx_napi_enabled))
			napi_enable(&priv->gfargrp[i].napi_tx);
	}
}

static int gfar_parse_group(struct device_node *np,
		struct gfar_private *priv, const char *model)
{
	u32 *queue_mask;

	priv->gfargrp[priv->num_grps].regs = of_iomap(np, 0);
	if (!priv->gfargrp[priv->num_grps].regs)
		return -ENOMEM;

	priv->gfargrp[priv->num_grps].interruptTransmit =
			irq_of_parse_and_map(np, 0);

	/* If we aren't the FEC we have multiple interrupts */
	if (model && strcasecmp(model, "FEC")) {
		priv->gfargrp[priv->num_grps].interruptReceive =
			irq_of_parse_and_map(np, 1);
		priv->gfargrp[priv->num_grps].interruptError =
			irq_of_parse_and_map(np,2);
		if (priv->gfargrp[priv->num_grps].interruptTransmit == NO_IRQ ||
		    priv->gfargrp[priv->num_grps].interruptReceive  == NO_IRQ ||
		    priv->gfargrp[priv->num_grps].interruptError    == NO_IRQ)
			return -EINVAL;
	}

	priv->gfargrp[priv->num_grps].grp_id = priv->num_grps;
	priv->gfargrp[priv->num_grps].priv = priv;
	spin_lock_init(&priv->gfargrp[priv->num_grps].grplock);
	if(priv->mode == MQ_MG_MODE) {
		queue_mask = (u32 *)of_get_property(np,
					"fsl,rx-bit-map", NULL);
		priv->gfargrp[priv->num_grps].rx_bit_map =
			queue_mask ?  *queue_mask :(DEFAULT_MAPPING >> priv->num_grps);
		queue_mask = (u32 *)of_get_property(np,
					"fsl,tx-bit-map", NULL);
		priv->gfargrp[priv->num_grps].tx_bit_map =
			queue_mask ? *queue_mask : (DEFAULT_MAPPING >> priv->num_grps);
	} else {
		priv->gfargrp[priv->num_grps].rx_bit_map = 0xFF;
		priv->gfargrp[priv->num_grps].tx_bit_map = 0xFF;
	}
	priv->num_grps++;

	return 0;
}

static int gfar_of_init(struct platform_device *ofdev, struct net_device **pdev)
{
	const char *model;
	const char *ctype;
	const void *mac_addr;
	int err = 0, i;
	struct net_device *dev = NULL;
	struct gfar_private *priv = NULL;
	struct device_node *np = ofdev->dev.of_node;
	struct device_node *child = NULL;
	const u32 *stash;
	const u32 *stash_len;
	const u32 *stash_idx;
	unsigned int num_tx_qs, num_rx_qs;
	u32 *tx_queues, *rx_queues;

	if (!np || !of_device_is_available(np))
		return -ENODEV;

	/* parse the num of tx and rx queues */
	tx_queues = (u32 *)of_get_property(np, "fsl,num_tx_queues", NULL);
	num_tx_qs = tx_queues ? *tx_queues : 1;

	if (num_tx_qs > MAX_TX_QS) {
		pr_err("num_tx_qs(=%d) greater than MAX_TX_QS(=%d)\n",
		       num_tx_qs, MAX_TX_QS);
		pr_err("Cannot do alloc_etherdev, aborting\n");
		return -EINVAL;
	}

	rx_queues = (u32 *)of_get_property(np, "fsl,num_rx_queues", NULL);
	num_rx_qs = rx_queues ? *rx_queues : 1;

	if (num_rx_qs > MAX_RX_QS) {
		pr_err("num_rx_qs(=%d) greater than MAX_RX_QS(=%d)\n",
		       num_rx_qs, MAX_RX_QS);
		pr_err("Cannot do alloc_etherdev, aborting\n");
		return -EINVAL;
	}

#ifdef CONFIG_RX_TX_BUFF_XCHG
	/* Creating multilple queues for avoiding lock in xmit function.*/
	num_tx_qs = (num_tx_qs < 3) ? 3 : num_tx_qs;
#endif

	*pdev = alloc_etherdev_mq(sizeof(*priv), num_tx_qs);
	dev = *pdev;
	if (NULL == dev)
		return -ENOMEM;

	priv = netdev_priv(dev);
	priv->node = ofdev->dev.of_node;
	priv->ndev = dev;

	priv->num_tx_queues = num_tx_qs;
	netif_set_real_num_rx_queues(dev, num_rx_qs);
	priv->num_rx_queues = num_rx_qs;
	priv->num_grps = 0x0;

	model = of_get_property(np, "model", NULL);

	for (i = 0; i < MAXGROUPS; i++)
		priv->gfargrp[i].regs = NULL;

	/* Parse and initialize group specific information */
	if (of_device_is_compatible(np, "fsl,etsec2")) {
		priv->mode = MQ_MG_MODE;
		for_each_child_of_node(np, child) {
			err = gfar_parse_group(child, priv, model);
			if (err)
				goto err_grp_init;
		}
	} else {
		priv->mode = SQ_SG_MODE;
		err = gfar_parse_group(np, priv, model);
		if(err)
			goto err_grp_init;
	}

	for (i = 0; i < priv->num_tx_queues; i++)
	       priv->tx_queue[i] = NULL;
	for (i = 0; i < priv->num_rx_queues; i++)
		priv->rx_queue[i] = NULL;

	for (i = 0; i < priv->num_tx_queues; i++) {
		priv->tx_queue[i] = kzalloc(sizeof(struct gfar_priv_tx_q),
					    GFP_KERNEL);
		if (!priv->tx_queue[i]) {
			err = -ENOMEM;
			goto tx_alloc_failed;
		}
		priv->tx_queue[i]->tx_skbuff = NULL;
		priv->tx_queue[i]->qindex = i;
		priv->tx_queue[i]->dev = dev;
		spin_lock_init(&(priv->tx_queue[i]->txlock));
	}

	for (i = 0; i < priv->num_rx_queues; i++) {
		priv->rx_queue[i] = kzalloc(sizeof(struct gfar_priv_rx_q),
					    GFP_KERNEL);
		if (!priv->rx_queue[i]) {
			err = -ENOMEM;
			goto rx_alloc_failed;
		}
		priv->rx_queue[i]->rx_skbuff = NULL;
		priv->rx_queue[i]->qindex = i;
		priv->rx_queue[i]->dev = dev;
		spin_lock_init(&(priv->rx_queue[i]->rxlock));
	}


	stash = of_get_property(np, "bd-stash", NULL);

	if (stash) {
		priv->device_flags |= FSL_GIANFAR_DEV_HAS_BD_STASHING;
		priv->bd_stash_en = 1;
	}

	stash_len = of_get_property(np, "rx-stash-len", NULL);

	if (stash_len)
		priv->rx_stash_size = *stash_len;

	stash_idx = of_get_property(np, "rx-stash-idx", NULL);

	if (stash_idx)
		priv->rx_stash_index = *stash_idx;

	if (stash_len || stash_idx)
		priv->device_flags |= FSL_GIANFAR_DEV_HAS_BUF_STASHING;

	mac_addr = of_get_mac_address(np);
	if (mac_addr)
		memcpy(dev->dev_addr, mac_addr, MAC_ADDR_LEN);

	if (model && !strcasecmp(model, "TSEC"))
		priv->device_flags =
			FSL_GIANFAR_DEV_HAS_GIGABIT |
			FSL_GIANFAR_DEV_HAS_COALESCE |
			FSL_GIANFAR_DEV_HAS_RMON |
			FSL_GIANFAR_DEV_HAS_MULTI_INTR;
	if (model && !strcasecmp(model, "eTSEC"))
		priv->device_flags =
			FSL_GIANFAR_DEV_HAS_GIGABIT |
			FSL_GIANFAR_DEV_HAS_COALESCE |
			FSL_GIANFAR_DEV_HAS_RMON |
			FSL_GIANFAR_DEV_HAS_MULTI_INTR |
			FSL_GIANFAR_DEV_HAS_PADDING |
			FSL_GIANFAR_DEV_HAS_CSUM |
			FSL_GIANFAR_DEV_HAS_VLAN |
			FSL_GIANFAR_DEV_HAS_EXTENDED_HASH |
			FSL_GIANFAR_DEV_HAS_36BIT_ADDR;

	ctype = of_get_property(np, "phy-connection-type", NULL);

	/* We only care about rgmii-id.  The rest are autodetected */
	if (ctype && !strcmp(ctype, "rgmii-id"))
		priv->interface = PHY_INTERFACE_MODE_RGMII_ID;
	else
		priv->interface = PHY_INTERFACE_MODE_MII;

	/* Init Wake-on-LAN */
	priv->wol_opts = 0;
	priv->wol_supported = 0;
#ifdef CONFIG_FSL_PMC
	if (of_get_property(np, "fsl,magic-packet", NULL)) {
		priv->device_flags |= FSL_GIANFAR_DEV_HAS_MAGIC_PACKET;
		priv->wol_supported |= GIANFAR_WOL_MAGIC;
	}

	if (of_get_property(np, "fsl,wake-on-filer", NULL)) {
		priv->device_flags |= FSL_GIANFAR_DEV_HAS_WAKE_ON_FILER;
		priv->wol_supported |= GIANFAR_WOL_ARP;
		priv->wol_supported |= GIANFAR_WOL_UCAST;
	}
#endif
	priv->phy_node = of_parse_phandle(np, "phy-handle", 0);

	/* Find the TBI PHY.  If it's not there, we don't support SGMII */
	priv->tbi_node = of_parse_phandle(np, "tbi-handle", 0);

	/* Handle IEEE1588 node */
	if (!gfar_ptp_init(np, priv))
		dev_info(&ofdev->dev, "ptp 1588 is initialized.\n");

	return 0;

rx_alloc_failed:
	free_rx_pointers(priv);
tx_alloc_failed:
	free_tx_pointers(priv);
err_grp_init:
	unmap_group_regs(priv);
	free_netdev(dev);
	return err;
}

static int gfar_hwtstamp_ioctl(struct net_device *netdev,
			struct ifreq *ifr, int cmd)
{
	struct hwtstamp_config config;
	struct gfar_private *priv = netdev_priv(netdev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	/* reserved for future extensions */
	if (config.flags)
		return -EINVAL;

	switch (config.tx_type) {
	case HWTSTAMP_TX_OFF:
		priv->hwts_tx_en = 0;
		/*
		 * remove RTPE bit - disable timestamp
		 * insertion on tx packets
		 */
		gfar_write(&(priv->ptimer->tmr_ctrl),
			gfar_read(&(priv->ptimer->tmr_ctrl))
					& (~TMR_RTPE));
		break;
	case HWTSTAMP_TX_ON:
		if (!(priv->device_flags & FSL_GIANFAR_DEV_HAS_TIMER))
			return -ERANGE;
		priv->hwts_tx_en = 1;
		/*
		 * add RTPE bit - enable timestamp insertion
		 * on tx packets
		 */
		gfar_write(&(priv->ptimer->tmr_ctrl),
			gfar_read(&(priv->ptimer->tmr_ctrl))
							| TMR_RTPE);
		break;
	default:
		return -ERANGE;
	}

	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		if (priv->hwts_rx_en) {
			stop_gfar(netdev);
			priv->hwts_rx_en = 0;
			gfar_write(&regs->rctrl,
				gfar_read(&regs->rctrl) & ~RCTRL_TS_ENABLE);
			startup_gfar(netdev);
		}
		break;
	default:
		if (!(priv->device_flags & FSL_GIANFAR_DEV_HAS_TIMER))
			return -ERANGE;
		if (!priv->hwts_rx_en) {
			stop_gfar(netdev);
			priv->hwts_rx_en = 1;
			gfar_write(&regs->rctrl,
				gfar_read(&regs->rctrl) | RCTRL_TS_ENABLE);
			startup_gfar(netdev);
		}
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		break;
	}

	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}

/* Ioctl MII Interface */
static int gfar_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct gfar_private *priv = netdev_priv(dev);

	if (!netif_running(dev))
		return -EINVAL;

	if (cmd == SIOCSHWTSTAMP)
		return gfar_hwtstamp_ioctl(dev, rq, cmd);

	if (!priv->phydev)
		return -ENODEV;

	if ((cmd >= PTP_ENBL_TXTS_IOCTL) &&
			(cmd <= PTP_CLEANUP_TS))
		return gfar_ioctl_1588(dev, rq, cmd);
	else
		return phy_mii_ioctl(priv->phydev, rq, cmd);
}

static unsigned int reverse_bitmap(unsigned int bit_map, unsigned int max_qs)
{
	unsigned int new_bit_map = 0x0;
	int mask = 0x1 << (max_qs - 1), i;
	for (i = 0; i < max_qs; i++) {
		if (bit_map & mask)
			new_bit_map = new_bit_map + (1 << i);
		mask = mask >> 0x1;
	}
	return new_bit_map;
}

#ifdef CONFIG_GFAR_HW_TCP_RECEIVE_OFFLOAD
void gfar_setup_hwaccel_tcp4_receive(struct sock *sk, struct sk_buff *skb)
{
	u32 rqfcr, rqfpr, rqidx;
	int i;
	struct tcphdr *th;
	struct iphdr *iph;
	struct gfar_private *priv = netdev_priv(skb->gfar_dev);
	struct gfar_hw_tcp_rcv_handle *hw_tcp = &priv->hw_tcp;

	if (!hw_tcp->en)
		return;

	i = hw_tcp->empty_chan_idx;
	hw_tcp->chan[i] = sk;
	/* keep the reference to this "channel" for sk_free() */
	sk->hw_tcp_chan_ref = &(hw_tcp->chan[i]);

	/* convert channel index to filer table index (4 entries per chan) */
	i = hw_tcp->filer_idx + (hw_tcp->empty_chan_idx * 4);

	/* setup the hw tcp channel */
	th = tcp_hdr(skb);
	iph = ip_hdr(skb);
	/* setup IPv4 source address */
	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_SIA | RQFCR_AND;
	rqfpr = ntohl(iph->saddr);
	priv->ftp_rqfcr[i] = rqfcr;
	priv->ftp_rqfpr[i] = rqfpr;
	gfar_write_filer(priv, i, rqfcr, rqfpr);
	i++;
	/* setup IPv4 destination address */
	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_DIA | RQFCR_AND;
	rqfpr = ntohl(iph->daddr);
	priv->ftp_rqfcr[i] = rqfcr;
	priv->ftp_rqfpr[i] = rqfpr;
	gfar_write_filer(priv, i, rqfcr, rqfpr);
	i++;
	/* setup TCP source port */
	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_SPT | RQFCR_AND;
	rqfpr = ntohs(th->source);
	priv->ftp_rqfcr[i] = rqfcr;
	priv->ftp_rqfpr[i] = rqfpr;
	gfar_write_filer(priv, i, rqfcr, rqfpr);
	i++;
	/* setup TCP destination port */
	rqidx = (GFAR_TCP_START_Q_IDX + hw_tcp->empty_chan_idx); /* set Q */
	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_DPT | (rqidx << 10);
	rqfpr = ntohs(th->dest);
	priv->ftp_rqfcr[i] = rqfcr;
	priv->ftp_rqfpr[i] = rqfpr;
	gfar_write_filer(priv, i, rqfcr, rqfpr);

	/* "round-robin" to the next empty hw tcp channel */
	i = (hw_tcp->empty_chan_idx + 1) % hw_tcp->chan_cnt;
	while (hw_tcp->chan[i] && (i != hw_tcp->empty_chan_idx))
		i = (i + 1) % hw_tcp->chan_cnt;
	/* if none found then take the next in line (and empty it) */
	if (i == hw_tcp->empty_chan_idx)
		i = (i + 1) % hw_tcp->chan_cnt;

	/* update the empty chan idx for the next hwaccel setup call */
	hw_tcp->empty_chan_idx = i;

	/* clean up the next in line tcp channel, if necessary */
	if (hw_tcp->chan[i]) {
		/* remove referece from corresp. sk to this "channel" */
		hw_tcp->chan[i]->hw_tcp_chan_ref = NULL;
		hw_tcp->chan[i] = NULL;

		/* convert channel index to filer table index */
		i = hw_tcp->filer_idx + (i * 4);

		/* clear the corresp. table entries */
		rqfcr = RQFCR_CMP_NOMATCH;
		rqfpr = FPR_FILER_MASK;
		priv->ftp_rqfcr[i] = rqfcr;
		priv->ftp_rqfpr[i] = rqfpr;
		gfar_write_filer(priv, i, rqfcr, rqfpr);
		i++;
		priv->ftp_rqfcr[i] = rqfcr;
		priv->ftp_rqfpr[i] = rqfpr;
		gfar_write_filer(priv, i, rqfcr, rqfpr);
		i++;
		priv->ftp_rqfcr[i] = rqfcr;
		priv->ftp_rqfpr[i] = rqfpr;
		gfar_write_filer(priv, i, rqfcr, rqfpr);
		i++;
		priv->ftp_rqfcr[i] = rqfcr;
		priv->ftp_rqfpr[i] = rqfpr;
		gfar_write_filer(priv, i, rqfcr, rqfpr);
	}
}

static u32 gfar_init_hw_tcp_cluster(struct gfar_private *priv, u32 rqfar)
{
	u32 rqfcr, rqfpr;
	int i, j;

	if (!priv->hw_tcp.en)
		return rqfar;
	/* 4 entries per channel, plus extra 4 for guard rule and clustering */
	i = rqfar - 4 * (priv->hw_tcp.chan_cnt + 1);
	if (i < 0)
		BUG();

	printk(KERN_INFO "%s: enabled hardware TCP receive offload\n",
			priv->ndev->name);

	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_MASK | RQFCR_AND;
	rqfpr = RQFPR_IPV4 | RQFPR_TCP;
	priv->ftp_rqfcr[i] = rqfcr;
	priv->ftp_rqfpr[i] = rqfpr;
	gfar_write_filer(priv, i, rqfcr, rqfpr);
	i++;
	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_PARSE | RQFCR_AND;
	rqfpr = RQFPR_IPV4 | RQFPR_TCP;
	priv->ftp_rqfcr[i] = rqfcr;
	priv->ftp_rqfpr[i] = rqfpr;
	gfar_write_filer(priv, i, rqfcr, rqfpr);
	i++;
	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_MASK | RQFCR_CLE | RQFCR_AND;
	rqfpr = FPR_FILER_MASK;
	priv->ftp_rqfcr[i] = rqfcr;
	priv->ftp_rqfpr[i] = rqfpr;
	gfar_write_filer(priv, i, rqfcr, rqfpr);
	i++;
	/* hold idx of the first channel's 1st entry */
	priv->hw_tcp.filer_idx = i;

	rqfcr = RQFCR_CMP_NOMATCH;
	rqfpr = FPR_FILER_MASK;
	for (j = 0; j < (priv->hw_tcp.chan_cnt * 4); j++) {
		priv->ftp_rqfcr[i] = rqfcr;
		priv->ftp_rqfpr[i] = rqfpr;
		gfar_write_filer(priv, i, rqfcr, rqfpr);
		i++;
	}

	rqfpr = FPR_FILER_MASK;
	rqfcr = RQFCR_CMP_NOMATCH | RQFCR_CLE;
	priv->ftp_rqfcr[i] = rqfcr;
	priv->ftp_rqfpr[i] = rqfpr;
	gfar_write_filer(priv, i, rqfcr, rqfpr);

	return rqfar - 4 * (priv->hw_tcp.chan_cnt + 1);
}
#endif

static u32 cluster_entry_per_class(struct gfar_private *priv, u32 rqfar,
				   u32 class)
{
	u32 rqfpr = FPR_FILER_MASK;
	u32 rqfcr = 0x0;

	rqfar--;
	rqfcr = RQFCR_CLE | RQFCR_PID_MASK | RQFCR_CMP_EXACT;
	priv->ftp_rqfpr[rqfar] = rqfpr;
	priv->ftp_rqfcr[rqfar] = rqfcr;
	gfar_write_filer(priv, rqfar, rqfcr, rqfpr);

	rqfar--;
	rqfcr = RQFCR_CMP_NOMATCH;
	priv->ftp_rqfpr[rqfar] = rqfpr;
	priv->ftp_rqfcr[rqfar] = rqfcr;
	gfar_write_filer(priv, rqfar, rqfcr, rqfpr);

	rqfar--;
	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_PARSE | RQFCR_CLE | RQFCR_AND;
	rqfpr = class;
	priv->ftp_rqfcr[rqfar] = rqfcr;
	priv->ftp_rqfpr[rqfar] = rqfpr;
	gfar_write_filer(priv, rqfar, rqfcr, rqfpr);

	rqfar--;
	rqfcr = RQFCR_CMP_EXACT | RQFCR_PID_MASK | RQFCR_AND;
	rqfpr = class;
	priv->ftp_rqfcr[rqfar] = rqfcr;
	priv->ftp_rqfpr[rqfar] = rqfpr;
	gfar_write_filer(priv, rqfar, rqfcr, rqfpr);

	return rqfar;
}

static void gfar_init_filer_table(struct gfar_private *priv)
{
	int i = 0x0;
	u32 rqfar = MAX_FILER_IDX;
	u32 rqfcr = 0x0;
	u32 rqfpr = FPR_FILER_MASK;

	/* Default rule */
	rqfcr = RQFCR_CMP_MATCH;
	priv->ftp_rqfcr[rqfar] = rqfcr;
	priv->ftp_rqfpr[rqfar] = rqfpr;
	gfar_write_filer(priv, rqfar, rqfcr, rqfpr);

	rqfar = cluster_entry_per_class(priv, rqfar, RQFPR_IPV6);
	rqfar = cluster_entry_per_class(priv, rqfar, RQFPR_IPV6 | RQFPR_UDP);
	rqfar = cluster_entry_per_class(priv, rqfar, RQFPR_IPV6 | RQFPR_TCP);
	rqfar = cluster_entry_per_class(priv, rqfar, RQFPR_IPV4);
	rqfar = cluster_entry_per_class(priv, rqfar, RQFPR_IPV4 | RQFPR_UDP);
	rqfar = cluster_entry_per_class(priv, rqfar, RQFPR_IPV4 | RQFPR_TCP);

#ifdef CONFIG_GFAR_HW_TCP_RECEIVE_OFFLOAD
	rqfar = gfar_init_hw_tcp_cluster(priv, rqfar);
#endif
	/* cur_filer_idx indicated the first non-masked rule */
	priv->cur_filer_idx = rqfar;

	/* Rest are masked rules */
	rqfcr = RQFCR_CMP_NOMATCH;
	for (i = 0; i < rqfar; i++) {
		priv->ftp_rqfcr[i] = rqfcr;
		priv->ftp_rqfpr[i] = rqfpr;
		gfar_write_filer(priv, i, rqfcr, rqfpr);
	}
}

static void gfar_detect_errata(struct gfar_private *priv)
{
	struct device *dev = &priv->ofdev->dev;
	unsigned int pvr = mfspr(SPRN_PVR);
	unsigned int svr = mfspr(SPRN_SVR);
	unsigned int mod = (svr >> 16) & 0xfff6; /* w/o E suffix */
	unsigned int rev = svr & 0xffff;

	/* MPC8313 Rev 2.0 and higher; All MPC837x */
	if ((pvr == 0x80850010 && mod == 0x80b0 && rev >= 0x0020) ||
			(pvr == 0x80861010 && (mod & 0xfff9) == 0x80c0))
		priv->errata |= GFAR_ERRATA_74;

	/* MPC8313 and MPC837x all rev */
	if ((pvr == 0x80850010 && mod == 0x80b0) ||
			(pvr == 0x80861010 && (mod & 0xfff9) == 0x80c0))
		priv->errata |= GFAR_ERRATA_76;

	/* MPC8313 and MPC837x all rev */
	if ((pvr == 0x80850010 && mod == 0x80b0) ||
			(pvr == 0x80861010 && (mod & 0xfff9) == 0x80c0))
		priv->errata |= GFAR_ERRATA_A002;

	/* MPC8313 Rev < 2.0, MPC8548 rev 2.0 */
	if ((pvr == 0x80850010 && mod == 0x80b0 && rev < 0x0020) ||
			(pvr == 0x80210020 && mod == 0x8030 && rev == 0x0020))
		priv->errata |= GFAR_ERRATA_12;

	if (priv->errata)
		dev_info(dev, "enabled errata workarounds, flags: 0x%x\n",
			 priv->errata);
}

/* Set up the ethernet device structure, private data,
 * and anything else we need before we start */
static int gfar_probe(struct platform_device *ofdev)
{
	u32 tempval;
	struct net_device *dev = NULL;
	struct gfar_private *priv = NULL;
	struct gfar __iomem *regs = NULL;
	int err = 0, i, grp_idx = 0;
	u32 rstat = 0, tstat = 0, rqueue = 0, tqueue = 0;
	u32 isrg = 0;
	u32 __iomem *baddr;

	err = gfar_of_init(ofdev, &dev);

	if (err)
		return err;

	priv = netdev_priv(dev);
	priv->ndev = dev;
	priv->ofdev = ofdev;
	priv->node = ofdev->dev.of_node;
	SET_NETDEV_DEV(dev, &ofdev->dev);

	spin_lock_init(&priv->bflock);
	INIT_WORK(&priv->reset_task, gfar_reset_task);

	dev_set_drvdata(&ofdev->dev, priv);
	regs = priv->gfargrp[0].regs;

	gfar_detect_errata(priv);

	/* Stop the DMA engine now, in case it was running before */
	/* (The firmware could have used it, and left it running). */
	gfar_halt(dev);

	/* Reset MAC layer */
	gfar_write(&regs->maccfg1, MACCFG1_SOFT_RESET);

	/* We need to delay at least 3 TX clocks */
	udelay(2);

	tempval = (MACCFG1_TX_FLOW | MACCFG1_RX_FLOW);
	gfar_write(&regs->maccfg1, tempval);

	/* Initialize MACCFG2. */
	tempval = MACCFG2_INIT_SETTINGS;
	if (gfar_has_errata(priv, GFAR_ERRATA_74))
		tempval |= MACCFG2_HUGEFRAME | MACCFG2_LENGTHCHECK;
	gfar_write(&regs->maccfg2, tempval);

	/* Initialize ECNTRL */
	gfar_write(&regs->ecntrl, ECNTRL_INIT_SETTINGS);

	/* Set the dev->base_addr to the gfar reg region */
	dev->base_addr = (unsigned long) regs;

	SET_NETDEV_DEV(dev, &ofdev->dev);

	/* Fill in the dev structure */
	dev->watchdog_timeo = TX_TIMEOUT;
	dev->mtu = 1500;
	dev->netdev_ops = &gfar_netdev_ops;
	dev->ethtool_ops = &gfar_ethtool_ops;

	/* Register for napi ...We are registering NAPI for each grp */
	if (rx_napi_weight != GFAR_DEV_RX_WEIGHT)
		if (rx_napi_weight < 0 || rx_napi_weight > GFAR_DEV_RX_WEIGHT)
			rx_napi_weight =  GFAR_DEV_RX_WEIGHT;

	if (tx_napi_enabled && tx_napi_weight != GFAR_DEV_TX_WEIGHT)
		if (tx_napi_weight < 0 || tx_napi_weight > GFAR_DEV_TX_WEIGHT)
			tx_napi_weight =  GFAR_DEV_TX_WEIGHT;

	for (i = 0; i < priv->num_grps; i++) {
		netif_napi_add(dev, &priv->gfargrp[i].napi_rx, gfar_poll_rx,
					rx_napi_weight);
		if (likely(tx_napi_enabled))
			netif_napi_add(dev, &priv->gfargrp[i].napi_tx,
					gfar_poll_tx,
					tx_napi_weight);
	}

	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_CSUM) {
		dev->hw_features = NETIF_F_IP_CSUM | NETIF_F_SG |
			NETIF_F_RXCSUM;
		dev->features |= NETIF_F_IP_CSUM | NETIF_F_SG |
			NETIF_F_RXCSUM | NETIF_F_HIGHDMA;
	}

	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_VLAN) {
		dev->hw_features |= NETIF_F_HW_VLAN_TX | NETIF_F_HW_VLAN_RX;
		/* HW VLAN insertion feature is disabled by default,
		 * but may be enabled via ethtool
		 */
		dev->features |= NETIF_F_HW_VLAN_RX;
	}

	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_EXTENDED_HASH) {
		priv->extended_hash = 1;
		priv->hash_width = 9;

		priv->hash_regs[0] = &regs->igaddr0;
		priv->hash_regs[1] = &regs->igaddr1;
		priv->hash_regs[2] = &regs->igaddr2;
		priv->hash_regs[3] = &regs->igaddr3;
		priv->hash_regs[4] = &regs->igaddr4;
		priv->hash_regs[5] = &regs->igaddr5;
		priv->hash_regs[6] = &regs->igaddr6;
		priv->hash_regs[7] = &regs->igaddr7;
		priv->hash_regs[8] = &regs->gaddr0;
		priv->hash_regs[9] = &regs->gaddr1;
		priv->hash_regs[10] = &regs->gaddr2;
		priv->hash_regs[11] = &regs->gaddr3;
		priv->hash_regs[12] = &regs->gaddr4;
		priv->hash_regs[13] = &regs->gaddr5;
		priv->hash_regs[14] = &regs->gaddr6;
		priv->hash_regs[15] = &regs->gaddr7;

	} else {
		priv->extended_hash = 0;
		priv->hash_width = 8;

		priv->hash_regs[0] = &regs->gaddr0;
		priv->hash_regs[1] = &regs->gaddr1;
		priv->hash_regs[2] = &regs->gaddr2;
		priv->hash_regs[3] = &regs->gaddr3;
		priv->hash_regs[4] = &regs->gaddr4;
		priv->hash_regs[5] = &regs->gaddr5;
		priv->hash_regs[6] = &regs->gaddr6;
		priv->hash_regs[7] = &regs->gaddr7;
	}

	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_PADDING)
		priv->padding = DEFAULT_PADDING;
	else
		priv->padding = 0;

	if (dev->features & NETIF_F_IP_CSUM ||
			priv->device_flags & FSL_GIANFAR_DEV_HAS_TIMER)
		dev->hard_header_len += GMAC_FCB_LEN;

	/* Program the isrg regs only if number of grps > 1 */
	if (priv->num_grps > 1) {
		baddr = &regs->isrg0;
		for (i = 0; i < priv->num_grps; i++) {
			isrg |= (priv->gfargrp[i].rx_bit_map << ISRG_SHIFT_RX);
			isrg |= (priv->gfargrp[i].tx_bit_map << ISRG_SHIFT_TX);
			gfar_write(baddr, isrg);
			baddr++;
			isrg = 0x0;
		}
	}

	/* Need to reverse the bit maps as  bit_map's MSB is q0
	 * but, for_each_set_bit parses from right to left, which
	 * basically reverses the queue numbers */
	for (i = 0; i< priv->num_grps; i++) {
		priv->gfargrp[i].tx_bit_map = reverse_bitmap(
				priv->gfargrp[i].tx_bit_map, MAX_TX_QS);
		priv->gfargrp[i].rx_bit_map = reverse_bitmap(
				priv->gfargrp[i].rx_bit_map, MAX_RX_QS);
	}

	/* Calculate RSTAT, TSTAT, RQUEUE and TQUEUE values,
	 * also assign queues to groups */
	for (grp_idx = 0; grp_idx < priv->num_grps; grp_idx++) {
		priv->gfargrp[grp_idx].num_rx_queues = 0x0;
		for_each_set_bit(i, &priv->gfargrp[grp_idx].rx_bit_map,
				priv->num_rx_queues) {
			priv->gfargrp[grp_idx].num_rx_queues++;
			priv->rx_queue[i]->grp = &priv->gfargrp[grp_idx];
			rstat = rstat | (RSTAT_CLEAR_RHALT >> i);
			rqueue = rqueue | ((RQUEUE_EN0 | RQUEUE_EX0) >> i);
		}
		priv->gfargrp[grp_idx].num_tx_queues = 0x0;
		for_each_set_bit(i, &priv->gfargrp[grp_idx].tx_bit_map,
				priv->num_tx_queues) {
			priv->gfargrp[grp_idx].num_tx_queues++;
			priv->tx_queue[i]->grp = &priv->gfargrp[grp_idx];
			tstat = tstat | (TSTAT_CLEAR_THALT >> i);
			tqueue = tqueue | (TQUEUE_EN0 >> i);
		}
		priv->gfargrp[grp_idx].rstat = rstat;
		priv->gfargrp[grp_idx].tstat = tstat;
		rstat = tstat =0;
	}

	gfar_write(&regs->rqueue, rqueue);
	gfar_write(&regs->tqueue, tqueue);

	priv->rx_buffer_size = DEFAULT_RX_BUFFER_SIZE;

	/* Initializing some of the rx/tx queue level parameters */
	for (i = 0; i < priv->num_tx_queues; i++) {
		priv->tx_queue[i]->tx_ring_size = DEFAULT_TX_RING_SIZE;
		priv->tx_queue[i]->num_txbdfree = DEFAULT_TX_RING_SIZE;
		priv->tx_queue[i]->txcoalescing = DEFAULT_TX_COALESCE;
		priv->tx_queue[i]->txic = DEFAULT_TXIC;
	}

	for (i = 0; i < priv->num_rx_queues; i++) {
		priv->rx_queue[i]->rx_ring_size = DEFAULT_RX_RING_SIZE;
		priv->rx_queue[i]->rxcoalescing = DEFAULT_RX_COALESCE;
		priv->rx_queue[i]->rxic = DEFAULT_RXIC;
	}

	/* enable filer if using multiple RX queues*/
	if(priv->num_rx_queues > 1)
		priv->rx_filer_enable = 1;
	/* Enable most messages by default */
	priv->msg_enable = (NETIF_MSG_IFUP << 1 ) - 1;
	/* use pritority h/w tx queue scheduling for single queue devices */
	if (priv->num_tx_queues == 1)
		priv->prio_sched_en = 1;

	/* Carrier starts down, phylib will bring it up */
	netif_carrier_off(dev);

	err = register_netdev(dev);

	if (err) {
		pr_err("%s: Cannot register net device, aborting\n", dev->name);
		goto register_fail;
	}

	if (priv->wol_supported) {
		device_set_wakeup_capable(&ofdev->dev, true);
		device_set_wakeup_enable(&ofdev->dev, false);
	}

	/* fill out IRQ number and name fields */
	for (i = 0; i < priv->num_grps; i++) {
		if (priv->device_flags & FSL_GIANFAR_DEV_HAS_MULTI_INTR) {
			sprintf(priv->gfargrp[i].int_name_tx, "%s%s%c%s",
				dev->name, "_g", '0' + i, "_tx");
			sprintf(priv->gfargrp[i].int_name_rx, "%s%s%c%s",
				dev->name, "_g", '0' + i, "_rx");
			sprintf(priv->gfargrp[i].int_name_er, "%s%s%c%s",
				dev->name, "_g", '0' + i, "_er");
		} else
			strcpy(priv->gfargrp[i].int_name_tx, dev->name);
	}

#ifdef CONFIG_GFAR_HW_TCP_RECEIVE_OFFLOAD
	/* set the number of hw_tcp channels */
	priv->hw_tcp.chan_cnt = (priv->num_rx_queues > GFAR_TCP_START_Q_IDX) \
				? (priv->num_rx_queues - GFAR_TCP_START_Q_IDX) \
				: 0;
	priv->hw_tcp.en = 1;
	/* we need at least 2 hw tcp channels for this feature */
	if (priv->hw_tcp.chan_cnt < 2 ||
		!(priv->ndev->features & NETIF_F_RXCSUM))
		priv->hw_tcp.en = 0;
	/* not a good idea to activate this feature if this gfar instance
	 * does not support it */
	if (!priv->hw_tcp.en)
		netdev_warn(dev,
			"H/W TCP receive offload not supported (disabled)!");
#endif

	/* Initialize the filer table */
	gfar_init_filer_table(priv);

	/* Create all the sysfs files */
	gfar_init_sysfs(dev);

	/* Print out the device info */
	netdev_info(dev, "mac: %pM\n", dev->dev_addr);

	/* Even more device info helps when determining which kernel */
	/* provided which set of benchmarks. */
	netdev_info(dev, "Running with NAPI enabled\n");
	for (i = 0; i < priv->num_rx_queues; i++)
		netdev_info(dev, "RX BD ring size for Q[%d]: %d\n",
			    i, priv->rx_queue[i]->rx_ring_size);
	for(i = 0; i < priv->num_tx_queues; i++)
		netdev_info(dev, "TX BD ring size for Q[%d]: %d\n",
			    i, priv->tx_queue[i]->tx_ring_size);

	return 0;

register_fail:
	gfar_ptp_cleanup(priv);
	unmap_group_regs(priv);
	free_tx_pointers(priv);
	free_rx_pointers(priv);
	if (priv->phy_node)
		of_node_put(priv->phy_node);
	if (priv->tbi_node)
		of_node_put(priv->tbi_node);
	free_netdev(dev);
	return err;
}

static int gfar_remove(struct platform_device *ofdev)
{
	struct gfar_private *priv = dev_get_drvdata(&ofdev->dev);

	if (priv->phy_node)
		of_node_put(priv->phy_node);
	if (priv->tbi_node)
		of_node_put(priv->tbi_node);

	dev_set_drvdata(&ofdev->dev, NULL);

	unregister_netdev(priv->ndev);
	unmap_group_regs(priv);
	free_netdev(priv->ndev);

	return 0;
}

#ifdef CONFIG_PM
static void gfar_enable_filer(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 temp;

	lock_rx_qs(priv);

	temp = gfar_read(&regs->rctrl);
	temp &= ~(RCTRL_FSQEN | RCTRL_PRSDEP_MASK);
	temp |= RCTRL_FILREN | RCTRL_PRSDEP_L2L3;
	gfar_write(&regs->rctrl, temp);

	unlock_rx_qs(priv);
}

static void gfar_disable_filer(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 temp;

	lock_rx_qs(priv);

	temp = gfar_read(&regs->rctrl);
	temp &= ~RCTRL_FILREN;
	gfar_write(&regs->rctrl, temp);

	unlock_rx_qs(priv);
}

static int gfar_get_ip(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct in_device *in_dev;
	int ret = -ENOENT;

	in_dev = in_dev_get(dev);
	if (!in_dev)
		return ret;

	/* Get the primary IP address */
	for_primary_ifa(in_dev) {
		priv->ip_addr = ifa->ifa_address;
		ret = 0;
		break;
	} endfor_ifa(in_dev);

	in_dev_put(in_dev);
	return ret;
}

static void gfar_restore_filer_table(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	u32 rqfcr, rqfpr;
	int i;

	lock_rx_qs(priv);

	for (i = 0; i <= MAX_FILER_IDX; i++) {
		rqfcr = priv->ftp_rqfcr[i];
		rqfpr = priv->ftp_rqfpr[i];
		gfar_write_filer(priv, i, rqfcr, rqfpr);
	}

	unlock_rx_qs(priv);
}

static void gfar_config_filer_table(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	u32 dest_mac_addr;
	u32 rqfcr, rqfpr;
	u8  rqfcr_queue = priv->num_rx_queues - 1;
	unsigned int index;

	if (gfar_get_ip(dev)) {
		netif_err(priv, wol, dev, "WOL: get the ip address error\n");
		return;
	}

	lock_rx_qs(priv);

	/* init filer table */
	rqfcr = RQFCR_RJE | RQFCR_CMP_MATCH;
	rqfpr = 0x0;
	for (index = 0; index <= MAX_FILER_IDX; index++)
		gfar_write_filer(priv, index, rqfcr, rqfpr);

	index = 0;
	if (priv->wol_opts & GIANFAR_WOL_ARP) {
		/* ARP request filer, filling the packet to the last queue */
		rqfcr = (rqfcr_queue << 10) | RQFCR_AND |
					RQFCR_CMP_EXACT | RQFCR_PID_MASK;
		rqfpr = RQFPR_ARQ;
		gfar_write_filer(priv, index++, rqfcr, rqfpr);

		rqfcr = (rqfcr_queue << 10) | RQFCR_AND |
					RQFCR_CMP_EXACT | RQFCR_PID_PARSE;
		rqfpr = RQFPR_ARQ;
		gfar_write_filer(priv, index++, rqfcr, rqfpr);

		/*
		 * DEST_IP address in ARP packet,
		 * filling it to the last queue.
		 */
		rqfcr = (rqfcr_queue << 10) | RQFCR_AND |
					RQFCR_CMP_EXACT | RQFCR_PID_MASK;
		rqfpr = FPR_FILER_MASK;
		gfar_write_filer(priv, index++, rqfcr, rqfpr);

		rqfcr = (rqfcr_queue << 10) | RQFCR_GPI |
					RQFCR_CMP_EXACT | RQFCR_PID_DIA;
		rqfpr = priv->ip_addr;
		gfar_write_filer(priv, index++, rqfcr, rqfpr);
	}

	if (priv->wol_opts & GIANFAR_WOL_UCAST) {
		/* Unicast packet, filling it to the last queue */
		dest_mac_addr = (dev->dev_addr[0] << 16) |
				(dev->dev_addr[1] << 8) | dev->dev_addr[2];
		rqfcr = (rqfcr_queue << 10) | RQFCR_AND |
					RQFCR_CMP_EXACT | RQFCR_PID_DAH;
		rqfpr = dest_mac_addr;
		gfar_write_filer(priv, index++, rqfcr, rqfpr);

		dest_mac_addr = (dev->dev_addr[3] << 16) |
				(dev->dev_addr[4] << 8) | dev->dev_addr[5];
		rqfcr = (rqfcr_queue << 10) | RQFCR_GPI |
					RQFCR_CMP_EXACT | RQFCR_PID_DAL;
		rqfpr = dest_mac_addr;
		gfar_write_filer(priv, index++, rqfcr, rqfpr);
	}

	unlock_rx_qs(priv);
}

static int gfar_suspend(struct device *dev)
{
	struct gfar_private *priv = dev_get_drvdata(dev);
	struct net_device *ndev = priv->ndev;
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	unsigned long flags;
	u32 tempval;

	netif_device_detach(ndev);

	if (!netif_running(ndev))
		return 0;

	local_irq_save(flags);
	lock_tx_qs(priv);
	lock_rx_qs(priv);

	gfar_halt(ndev);

	unlock_rx_qs(priv);
	unlock_tx_qs(priv);
	local_irq_restore(flags);

	disable_napi(priv);

	if (!priv->wol_opts && priv->phydev) {
		phy_stop(priv->phydev);
		return 0;
	}

	mpc85xx_pmc_set_wake(priv->ofdev, 1);
	if (priv->wol_opts & GIANFAR_WOL_MAGIC) {
		/* Enable Magic Packet mode */
		tempval = gfar_read(&regs->maccfg2);
		tempval |= MACCFG2_MPEN;
		gfar_write(&regs->maccfg2, tempval);
	}

	if (priv->wol_opts & (GIANFAR_WOL_ARP | GIANFAR_WOL_UCAST)) {
		mpc85xx_pmc_set_lossless_ethernet(1);
		gfar_disable_filer(ndev);
		gfar_config_filer_table(ndev);
		gfar_enable_filer(ndev);
	}
	gfar_rx_start(ndev);
	return 0;
}

static int gfar_resume(struct device *dev)
{
	struct gfar_private *priv = dev_get_drvdata(dev);
	struct net_device *ndev = priv->ndev;
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	unsigned long flags;
	u32 tempval, i;

	if (!netif_running(ndev)) {
		netif_device_attach(ndev);
		return 0;
	}

	if (!priv->wol_opts && priv->phydev) {
		phy_start(priv->phydev);
		goto out;
	}

	mpc85xx_pmc_set_wake(priv->ofdev, 0);

	local_irq_save(flags);
	lock_rx_qs(priv);
	gfar_halt_rx(ndev);
	unlock_rx_qs(priv);
	local_irq_restore(flags);

	if (priv->wol_opts & (GIANFAR_WOL_ARP | GIANFAR_WOL_UCAST)) {
		mpc85xx_pmc_set_lossless_ethernet(0);
		gfar_disable_filer(ndev);
		gfar_restore_filer_table(ndev);
	}

	if (priv->wol_opts & GIANFAR_WOL_MAGIC) {
		/* Disable Magic Packet mode */
		tempval = gfar_read(&regs->maccfg2);
		tempval &= ~MACCFG2_MPEN;
		gfar_write(&regs->maccfg2, tempval);
	}

out:
	gfar_start(ndev);
	netif_device_attach(ndev);
	enable_napi(priv);

	if (priv->wol_opts & (GIANFAR_WOL_ARP | GIANFAR_WOL_UCAST)) {
		/* send requests to process the received packets */
		for (i = 0; i < priv->num_grps; i++)
			gfar_schedule_rx_cleanup(&priv->gfargrp[i]);
	}
	return 0;
}

static int gfar_restore(struct device *dev)
{
	struct gfar_private *priv = dev_get_drvdata(dev);
	struct net_device *ndev = priv->ndev;

	if (!netif_running(ndev))
		return 0;

	gfar_init_bds(ndev);
	init_registers(ndev);
	gfar_set_mac_address(ndev);
	gfar_init_mac(ndev);
	gfar_start(ndev);

	priv->oldlink = 0;
	priv->oldspeed = 0;
	priv->oldduplex = -1;

	if (priv->phydev)
		phy_start(priv->phydev);

	netif_device_attach(ndev);
	enable_napi(priv);

	return 0;
}

static struct dev_pm_ops gfar_pm_ops = {
	.suspend = gfar_suspend,
	.resume = gfar_resume,
	.freeze = gfar_suspend,
	.thaw = gfar_resume,
	.restore = gfar_restore,
};

#define GFAR_PM_OPS (&gfar_pm_ops)

#else

#define GFAR_PM_OPS NULL

#endif

/* Reads the controller's registers to determine what interface
 * connects it to the PHY.
 */
static phy_interface_t gfar_get_interface(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 ecntrl;

	ecntrl = gfar_read(&regs->ecntrl);

	if (ecntrl & ECNTRL_SGMII_MODE)
		return PHY_INTERFACE_MODE_SGMII;

	if (ecntrl & ECNTRL_TBI_MODE) {
		if (ecntrl & ECNTRL_REDUCED_MODE)
			return PHY_INTERFACE_MODE_RTBI;
		else
			return PHY_INTERFACE_MODE_TBI;
	}

	if (ecntrl & ECNTRL_REDUCED_MODE) {
		if (ecntrl & ECNTRL_REDUCED_MII_MODE)
			return PHY_INTERFACE_MODE_RMII;
		else {
			phy_interface_t interface = priv->interface;

			/*
			 * This isn't autodetected right now, so it must
			 * be set by the device tree or platform code.
			 */
			if (interface == PHY_INTERFACE_MODE_RGMII_ID)
				return PHY_INTERFACE_MODE_RGMII_ID;

			return PHY_INTERFACE_MODE_RGMII;
		}
	}

	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_GIGABIT)
		return PHY_INTERFACE_MODE_GMII;

	return PHY_INTERFACE_MODE_MII;
}


/* Initializes driver's PHY state, and attaches to the PHY.
 * Returns 0 on success.
 */
static int init_phy(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	uint gigabit_support =
		priv->device_flags & FSL_GIANFAR_DEV_HAS_GIGABIT ?
		SUPPORTED_1000baseT_Full : 0;
	phy_interface_t interface;

	priv->oldlink = 0;
	priv->oldspeed = 0;
	priv->oldduplex = -1;

	interface = gfar_get_interface(dev);

	priv->phydev = of_phy_connect(dev, priv->phy_node, &adjust_link, 0,
				      interface);
	if (!priv->phydev)
		priv->phydev = of_phy_connect_fixed_link(dev, &adjust_link,
							 interface);
	if (!priv->phydev) {
		dev_err(&dev->dev, "could not attach to PHY\n");
		return -ENODEV;
	}

	if (interface == PHY_INTERFACE_MODE_SGMII)
		gfar_configure_serdes(dev);

	/* Remove any features not supported by the controller */
	priv->phydev->supported &= (GFAR_SUPPORTED | gigabit_support);
	priv->phydev->advertising = priv->phydev->supported;

	return 0;
}

/*
 * Initialize TBI PHY interface for communicating with the
 * SERDES lynx PHY on the chip.  We communicate with this PHY
 * through the MDIO bus on each controller, treating it as a
 * "normal" PHY at the address found in the TBIPA register.  We assume
 * that the TBIPA register is valid.  Either the MDIO bus code will set
 * it to a value that doesn't conflict with other PHYs on the bus, or the
 * value doesn't matter, as there are no other PHYs on the bus.
 */
static void gfar_configure_serdes(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct phy_device *tbiphy;

	if (!priv->tbi_node) {
		dev_warn(&dev->dev, "error: SGMII mode requires that the "
				    "device tree specify a tbi-handle\n");
		return;
	}

	tbiphy = of_phy_find_device(priv->tbi_node);
	if (!tbiphy) {
		dev_err(&dev->dev, "error: Could not get TBI device\n");
		return;
	}

	/*
	 * If the link is already up, we must already be ok, and don't need to
	 * configure and reset the TBI<->SerDes link.  Maybe U-Boot configured
	 * everything for us?  Resetting it takes the link down and requires
	 * several seconds for it to come back.
	 */
	if (phy_read(tbiphy, MII_BMSR) & BMSR_LSTATUS)
		return;

	/* Single clk mode, mii mode off(for serdes communication) */
	phy_write(tbiphy, MII_TBICON, TBICON_CLK_SELECT);

	phy_write(tbiphy, MII_ADVERTISE,
			ADVERTISE_1000XFULL | ADVERTISE_1000XPAUSE |
			ADVERTISE_1000XPSE_ASYM);

	phy_write(tbiphy, MII_BMCR, BMCR_ANENABLE |
			BMCR_ANRESTART | BMCR_FULLDPLX | BMCR_SPEED1000);
}

static void init_registers(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = NULL;
	int i = 0;

	for (i = 0; i < priv->num_grps; i++) {
		regs = priv->gfargrp[i].regs;
		/* Clear IEVENT */
		gfar_write(&regs->ievent, IEVENT_INIT_CLEAR);

		/* Initialize IMASK */
		gfar_write(&regs->imask, IMASK_INIT_CLEAR);
	}

	regs = priv->gfargrp[0].regs;
	/* Init hash registers to zero */
	gfar_write(&regs->igaddr0, 0);
	gfar_write(&regs->igaddr1, 0);
	gfar_write(&regs->igaddr2, 0);
	gfar_write(&regs->igaddr3, 0);
	gfar_write(&regs->igaddr4, 0);
	gfar_write(&regs->igaddr5, 0);
	gfar_write(&regs->igaddr6, 0);
	gfar_write(&regs->igaddr7, 0);

	gfar_write(&regs->gaddr0, 0);
	gfar_write(&regs->gaddr1, 0);
	gfar_write(&regs->gaddr2, 0);
	gfar_write(&regs->gaddr3, 0);
	gfar_write(&regs->gaddr4, 0);
	gfar_write(&regs->gaddr5, 0);
	gfar_write(&regs->gaddr6, 0);
	gfar_write(&regs->gaddr7, 0);

	/* Zero out the rmon mib registers if it has them */
	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_RMON) {
		memset_io(&(regs->rmon), 0, sizeof (struct rmon_mib));

		/* Mask off the CAM interrupts */
		gfar_write(&regs->rmon.cam1, 0xffffffff);
		gfar_write(&regs->rmon.cam2, 0xffffffff);
	}

	/* Initialize the max receive buffer length */
	gfar_write(&regs->mrblr, priv->rx_buffer_size);

	/* Initialize the Minimum Frame Length Register */
	gfar_write(&regs->minflr, MINFLR_INIT_SETTINGS);
}

static int __gfar_is_rx_idle(struct gfar_private *priv)
{
	u32 res;

	/*
	 * Normaly TSEC should not hang on GRS commands, so we should
	 * actually wait for IEVENT_GRSC flag.
	 */
	if (likely(!gfar_has_errata(priv, GFAR_ERRATA_A002)))
		return 0;

	/*
	 * Read the eTSEC register at offset 0xD1C. If bits 7-14 are
	 * the same as bits 23-30, the eTSEC Rx is assumed to be idle
	 * and the Rx can be safely reset.
	 */
	res = gfar_read((void __iomem *)priv->gfargrp[0].regs + 0xd1c);
	res &= 0x7f807f80;
	if ((res & 0xffff) == (res >> 16))
		return 1;

	return 0;
}

#ifdef CONFIG_PM
/* Halt the receive queues */
static void gfar_halt_rx(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 tempval;
	int i = 0;

	for (i = 0; i < priv->num_grps; i++) {
		regs = priv->gfargrp[i].regs;
		/* Mask all interrupts */
		gfar_write(&regs->imask, IMASK_INIT_CLEAR);

		/* Clear all interrupts */
		gfar_write(&regs->ievent, IEVENT_INIT_CLEAR);
	}

	regs = priv->gfargrp[0].regs;
	/* Stop the DMA, and wait for it to stop */
	tempval = gfar_read(&regs->dmactrl);
	if ((tempval & DMACTRL_GRS) != DMACTRL_GRS) {
		int ret;

		tempval |= DMACTRL_GRS;
		gfar_write(&regs->dmactrl, tempval);

		do {
			ret = spin_event_timeout(((gfar_read(&regs->ievent) &
				IEVENT_GRSC) == IEVENT_GRSC), 1000000, 0);
			if (!ret && !(gfar_read(&regs->ievent) & IEVENT_GRSC))
				ret = __gfar_is_rx_idle(priv);
		} while (!ret);
	}

	/* Disable Rx in MACCFG1  */
	tempval = gfar_read(&regs->maccfg1);
	tempval &= ~MACCFG1_RX_EN;
	gfar_write(&regs->maccfg1, tempval);
}
#endif

/* Halt the receive and transmit queues */
static void gfar_halt_nodisable(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = NULL;
	u32 tempval;
	int i = 0;

	for (i = 0; i < priv->num_grps; i++) {
		regs = priv->gfargrp[i].regs;
		/* Mask all interrupts */
		gfar_write(&regs->imask, IMASK_INIT_CLEAR);

		/* Clear all interrupts */
		gfar_write(&regs->ievent, IEVENT_INIT_CLEAR);
	}

	regs = priv->gfargrp[0].regs;
	/* Stop the DMA, and wait for it to stop */
	tempval = gfar_read(&regs->dmactrl);
	if ((tempval & (DMACTRL_GRS | DMACTRL_GTS))
	    != (DMACTRL_GRS | DMACTRL_GTS)) {
		int ret;

		tempval |= (DMACTRL_GRS | DMACTRL_GTS);
		gfar_write(&regs->dmactrl, tempval);

		do {
			ret = spin_event_timeout(((gfar_read(&regs->ievent) &
				 (IEVENT_GRSC | IEVENT_GTSC)) ==
				 (IEVENT_GRSC | IEVENT_GTSC)), 1000000, 0);
			if (!ret && !(gfar_read(&regs->ievent) & IEVENT_GRSC))
				ret = __gfar_is_rx_idle(priv);
		} while (!ret);
	}
}

/* Halt the receive and transmit queues */
void gfar_halt(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 tempval;

	gfar_halt_nodisable(dev);

	/* Disable Rx and Tx */
	tempval = gfar_read(&regs->maccfg1);
	tempval &= ~(MACCFG1_RX_EN | MACCFG1_TX_EN);
	gfar_write(&regs->maccfg1, tempval);
}

static void free_grp_irqs(struct gfar_priv_grp *grp)
{
	free_irq(grp->interruptError, grp);
	free_irq(grp->interruptTransmit, grp);
	free_irq(grp->interruptReceive, grp);
}

void stop_gfar(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	unsigned long flags;
	int i;

	phy_stop(priv->phydev);


	/* Lock it down */
	local_irq_save_nort(flags);
	lock_tx_qs(priv);
	lock_rx_qs(priv);

	gfar_halt(dev);

	unlock_rx_qs(priv);
	unlock_tx_qs(priv);
	local_irq_restore_nort(flags);

	/* Free the IRQs */
	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_MULTI_INTR) {
		for (i = 0; i < priv->num_grps; i++)
			free_grp_irqs(&priv->gfargrp[i]);
	} else {
		for (i = 0; i < priv->num_grps; i++)
			free_irq(priv->gfargrp[i].interruptTransmit,
					&priv->gfargrp[i]);
	}

	free_skb_resources(priv);
}

static void free_skb_tx_queue(struct gfar_priv_tx_q *tx_queue)
{
	struct txbd8 *txbdp;
	struct gfar_private *priv = netdev_priv(tx_queue->dev);
	int i, j;

	txbdp = tx_queue->tx_bd_base;

	for (i = 0; i < tx_queue->tx_ring_size; i++) {
		if (!tx_queue->tx_skbuff[i])
			continue;

		dma_unmap_single(&priv->ofdev->dev, txbdp->bufPtr,
				txbdp->length, DMA_TO_DEVICE);
		txbdp->lstatus = 0;
		for (j = 0; j < skb_shinfo(tx_queue->tx_skbuff[i])->nr_frags;
				j++) {
			txbdp++;
			dma_unmap_page(&priv->ofdev->dev, txbdp->bufPtr,
					txbdp->length, DMA_TO_DEVICE);
		}
		txbdp++;
		dev_kfree_skb_any(tx_queue->tx_skbuff[i]);
		tx_queue->tx_skbuff[i] = NULL;
	}
	kfree(tx_queue->tx_skbuff);
}

static void free_skb_rx_queue(struct gfar_priv_rx_q *rx_queue)
{
	struct rxbd8 *rxbdp;
	struct gfar_private *priv = netdev_priv(rx_queue->dev);
	int i;

	rxbdp = rx_queue->rx_bd_base;

	for (i = 0; i < rx_queue->rx_ring_size; i++) {
		if (rx_queue->rx_skbuff[i]) {
			dma_unmap_single(&priv->ofdev->dev,
					rxbdp->bufPtr, priv->rx_buffer_size,
					DMA_FROM_DEVICE);
			dev_kfree_skb_any(rx_queue->rx_skbuff[i]);
			rx_queue->rx_skbuff[i] = NULL;
		}
		rxbdp->lstatus = 0;
		rxbdp->bufPtr = 0;
		rxbdp++;
	}
	kfree(rx_queue->rx_skbuff);
}

static void gfar_free_bds(struct gfar_private *priv)
{
#ifdef CONFIG_GIANFAR_L2SRAM
	if (priv->l2sram_bds_en) {
		mpc85xx_cache_sram_free(priv->tx_queue[0]->tx_bd_base);
		return;
	}
#endif
	dma_free_coherent(&priv->ofdev->dev,
			BDS_REGION_SIZE(priv),
			priv->tx_queue[0]->tx_bd_base,
			priv->tx_queue[0]->tx_bd_dma_base);
}

/* If there are any tx skbs or rx skbs still around, free them.
 * Then free tx_skbuff and rx_skbuff */
static void free_skb_resources(struct gfar_private *priv)
{
	struct gfar_priv_tx_q *tx_queue = NULL;
	struct gfar_priv_rx_q *rx_queue = NULL;
	int i;

	/* Go through all the buffer descriptors and free their data buffers */
	for (i = 0; i < priv->num_tx_queues; i++) {
		tx_queue = priv->tx_queue[i];
		if(tx_queue->tx_skbuff)
			free_skb_tx_queue(tx_queue);
	}

	for (i = 0; i < priv->num_rx_queues; i++) {
		rx_queue = priv->rx_queue[i];
		if(rx_queue->rx_skbuff)
			free_skb_rx_queue(rx_queue);
	}
	gfar_free_bds(priv);
}

void gfar_start(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 tempval;
	int i = 0;

	/* Enable Rx and Tx in MACCFG1 */
	tempval = gfar_read(&regs->maccfg1);
	tempval |= (MACCFG1_RX_EN | MACCFG1_TX_EN);
	gfar_write(&regs->maccfg1, tempval);

	/* Initialize DMACTRL to have WWR and WOP */
	tempval = gfar_read(&regs->dmactrl);
	tempval |= DMACTRL_INIT_SETTINGS;
	gfar_write(&regs->dmactrl, tempval);

	/* Make sure we aren't stopped */
	tempval = gfar_read(&regs->dmactrl);
	tempval &= ~(DMACTRL_GRS | DMACTRL_GTS);
	gfar_write(&regs->dmactrl, tempval);

	for (i = 0; i < priv->num_grps; i++) {
		regs = priv->gfargrp[i].regs;
		/* Clear THLT/RHLT, so that the DMA starts polling now */
		gfar_write(&regs->tstat, priv->gfargrp[i].tstat);
		gfar_write(&regs->rstat, priv->gfargrp[i].rstat);
		/* Unmask the interrupts we look for */
		gfar_write(&regs->imask, IMASK_DEFAULT);
	}

	dev->trans_start = jiffies; /* prevent tx timeout */
}

#ifdef CONFIG_PM
void gfar_rx_start(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 tempval;
	int i = 0;

	/* Enable Rx in MACCFG1 */
	tempval = gfar_read(&regs->maccfg1);
	tempval |= MACCFG1_RX_EN;
	gfar_write(&regs->maccfg1, tempval);

	/* Initialize DMACTRL to have WWR and WOP */
	tempval = gfar_read(&regs->dmactrl);
	tempval |= DMACTRL_INIT_SETTINGS;
	gfar_write(&regs->dmactrl, tempval);

	/* Make sure we aren't stopped */
	tempval = gfar_read(&regs->dmactrl);
	tempval &= ~DMACTRL_GRS;
	gfar_write(&regs->dmactrl, tempval);

	for (i = 0; i < priv->num_grps; i++) {
		regs = priv->gfargrp[i].regs;
		/* Clear RHLT, so that the DMA starts polling now */
		gfar_write(&regs->rstat, priv->gfargrp[i].rstat);

		/* Unmask the interrupts we look for */
		gfar_write(&regs->imask, IMASK_DEFAULT);
	}
}
#endif

void gfar_configure_tx_coalescing(struct gfar_private *priv,
				unsigned long tx_mask)
{
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 __iomem *baddr;
	int i = 0, mask = 0x1;

	/* Backward compatible case ---- even if we enable
	 * multiple queues, there's only single reg to program
	 */
	if (priv->mode == SQ_SG_MODE) {
		gfar_write(&regs->txic, 0);
		if (likely(priv->tx_queue[0]->txcoalescing))
			gfar_write(&regs->txic, priv->tx_queue[0]->txic);
	}

	if (priv->mode == MQ_MG_MODE) {
		baddr = &regs->txic0;
		for (i = 0; i < priv->num_tx_queues; i++) {
			gfar_write(baddr + i, 0);
			if (tx_mask & mask)
				if (likely(priv->tx_queue[i]->txcoalescing))
					gfar_write(baddr + i,
						 priv->tx_queue[i]->txic);
			mask = mask << 0x1;
		}
	}
}

void gfar_configure_rx_coalescing(struct gfar_private *priv,
				unsigned long rx_mask)
{
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 __iomem *baddr;
	int i = 0, mask = 0x1;

	/* Backward compatible case ---- even if we enable
	 * multiple queues, there's only single reg to program
	 */
	if (priv->mode == SQ_SG_MODE) {
		gfar_write(&regs->rxic, 0);
		if (unlikely(priv->rx_queue[0]->rxcoalescing))
			gfar_write(&regs->rxic, priv->rx_queue[0]->rxic);
	}

	if (priv->mode == MQ_MG_MODE) {
		baddr = &regs->rxic0;
		for (i = 0; i < priv->num_rx_queues; i++) {
			gfar_write(baddr + i, 0);
			if (rx_mask & mask)
				if (likely(priv->rx_queue[i]->rxcoalescing))
					gfar_write(baddr + i,
						priv->rx_queue[i]->rxic);
			mask = mask << 0x1;
		}
	}
}

static int register_grp_irqs(struct gfar_priv_grp *grp)
{
	struct gfar_private *priv = grp->priv;
	struct net_device *dev = priv->ndev;
	int err;
	unsigned long flags = priv->wol_supported ? IRQF_NO_SUSPEND : 0;

	/* If the device has multiple interrupts, register for
	 * them.  Otherwise, only register for the one */
	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_MULTI_INTR) {
		/* Install our interrupt handlers for Error,
		 * Transmit, and Receive */
		err = request_irq(grp->interruptError, gfar_error, flags,
				  grp->int_name_er, grp);
		if (err < 0) {
			netif_err(priv, intr, dev, "Can't get IRQ %d\n",
				  grp->interruptError);

			goto err_irq_fail;
		}

#ifndef CONFIG_RX_TX_BUFF_XCHG
		if (likely(tx_napi_enabled)) {
			err = request_irq(grp->interruptTransmit,
					gfar_transmit, 0,
					grp->int_name_tx, grp);
		} else {
			err = request_irq(grp->interruptTransmit,
					gfar_transmit_no_napi, 0,
					grp->int_name_tx, grp);
		}
#else
		err = request_irq(grp->interruptTransmit,
					gfar_enable_tx_queue, 0,
					grp->int_name_tx, grp);
#endif

		if (err < 0) {
			netif_err(priv, intr, dev, "Can't get IRQ %d\n",
				  grp->interruptTransmit);
			goto tx_irq_fail;
		}

		err = request_irq(grp->interruptReceive, gfar_receive,
				  flags, grp->int_name_rx, grp);
		if (err < 0) {
			netif_err(priv, intr, dev, "Can't get IRQ %d\n",
				  grp->interruptReceive);
			goto rx_irq_fail;
		}
	} else {
		err = request_irq(grp->interruptTransmit, gfar_interrupt,
				  flags, grp->int_name_tx, grp);
		if (err < 0) {
			netif_err(priv, intr, dev, "Can't get IRQ %d\n",
				  grp->interruptTransmit);
			goto err_irq_fail;
		}
	}

	return 0;

rx_irq_fail:
	free_irq(grp->interruptTransmit, grp);
tx_irq_fail:
	free_irq(grp->interruptError, grp);
err_irq_fail:
	return err;

}

/* Bring the controller up and running */
int startup_gfar(struct net_device *ndev)
{
	struct gfar_private *priv = netdev_priv(ndev);
	struct gfar __iomem *regs = NULL;
	int err, i, j;

	for (i = 0; i < priv->num_grps; i++) {
		regs= priv->gfargrp[i].regs;
		gfar_write(&regs->imask, IMASK_INIT_CLEAR);
	}

	regs= priv->gfargrp[0].regs;
	err = gfar_alloc_skb_resources(ndev);
	if (err)
		return err;

	gfar_init_mac(ndev);

	for (i = 0; i < priv->num_grps; i++) {
		err = register_grp_irqs(&priv->gfargrp[i]);
		if (err) {
			for (j = 0; j < i; j++)
				free_grp_irqs(&priv->gfargrp[j]);
			goto irq_fail;
		}
	}

	/* Start the controller */
	gfar_start(ndev);

	phy_start(priv->phydev);

	gfar_configure_tx_coalescing(priv, 0xFF);
	gfar_configure_rx_coalescing(priv, 0xFF);

	return 0;

irq_fail:
	free_skb_resources(priv);
	return err;
}

void __exit gfar_free_recycle_cntxt(struct gfar_recycle_cntxt *recycle_cntxt)
{
	struct gfar_recycle_cntxt_percpu *local;
	int cpu;

	if (!recycle_cntxt)
		return;
	if (!recycle_cntxt->global_recycle_q)
		return;
	skb_queue_purge(recycle_cntxt->global_recycle_q);
	kfree(recycle_cntxt->global_recycle_q);
	if (!recycle_cntxt->local)
		return;
	for_each_possible_cpu(cpu) {
		local = per_cpu_ptr(recycle_cntxt->local, cpu);
		if (!local->recycle_q)
			continue;
		skb_queue_purge(local->recycle_q);
		kfree(local->recycle_q);
	}
	free_percpu(recycle_cntxt->local);
	kfree(recycle_cntxt);
}

struct gfar_recycle_cntxt *__init gfar_init_recycle_cntxt(void)
{
	struct gfar_recycle_cntxt *recycle_cntxt;
	struct gfar_recycle_cntxt_percpu *local;
	int cpu;

	recycle_cntxt = kzalloc(sizeof(struct gfar_recycle_cntxt),
							GFP_KERNEL);
	if (!recycle_cntxt)
		goto err;

	recycle_cntxt->recycle_max = GFAR_RX_RECYCLE_MAX;
	spin_lock_init(&recycle_cntxt->recycle_lock);
	recycle_cntxt->global_recycle_q = kmalloc(sizeof(struct sk_buff_head),
							GFP_KERNEL);
	if (!recycle_cntxt->global_recycle_q)
		goto err;
	skb_queue_head_init(recycle_cntxt->global_recycle_q);

	recycle_cntxt->local = alloc_percpu(struct gfar_recycle_cntxt_percpu);
	if (!recycle_cntxt->local)
		goto err;
	for_each_possible_cpu(cpu) {
		local = per_cpu_ptr(recycle_cntxt->local, cpu);
		local->recycle_q = kmalloc(sizeof(struct sk_buff_head),
							GFP_KERNEL);
		if (!local->recycle_q)
			goto err;
		skb_queue_head_init(local->recycle_q);
	}

	return recycle_cntxt;
err:
	gfar_free_recycle_cntxt(recycle_cntxt);
	return NULL;
}

/* Called when something needs to use the ethernet device */
/* Returns 0 for success. */
static int gfar_enet_open(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	int err;

	enable_napi(priv);

	priv->recycle = gfar_global_recycle_cntxt;

	/* Initialize a bunch of registers */
	init_registers(dev);

	gfar_set_mac_address(dev);

	err = init_phy(dev);

	if (err) {
		disable_napi(priv);
		return err;
	}

	err = startup_gfar(dev);
	if (err) {
		disable_napi(priv);
		return err;
	}

	netif_tx_start_all_queues(dev);

	device_set_wakeup_enable(&priv->ofdev->dev, priv->wol_opts);

	return err;
}

static inline struct txfcb *gfar_add_fcb(struct sk_buff *skb)
{
	struct txfcb *fcb = (struct txfcb *)skb_push(skb, GMAC_FCB_LEN);

	memset(fcb, 0, GMAC_FCB_LEN);

	return fcb;
}

static inline void gfar_tx_checksum(struct sk_buff *skb, struct txfcb *fcb)
{
	u8 flags = 0;

	/* If we're here, it's a IP packet with a TCP or UDP
	 * payload.  We set it to checksum, using a pseudo-header
	 * we provide
	 */
	flags = TXFCB_DEFAULT;

	/* Tell the controller what the protocol is */
	/* And provide the already calculated phcs */
	if (ip_hdr(skb)->protocol == IPPROTO_UDP) {
		flags |= TXFCB_UDP;
		fcb->phcs = udp_hdr(skb)->check;
	} else
		fcb->phcs = tcp_hdr(skb)->check;

	/* l3os is the distance between the start of the
	 * frame (skb->data) and the start of the IP hdr.
	 * l4os is the distance between the start of the
	 * l3 hdr and the l4 hdr */
	fcb->l3os = (u16)(skb_network_offset(skb) - GMAC_FCB_LEN);
	fcb->l4os = skb_network_header_len(skb);

	fcb->flags = flags;
}

void inline gfar_tx_vlan(struct sk_buff *skb, struct txfcb *fcb)
{
	fcb->flags |= TXFCB_VLN;
	fcb->vlctl = vlan_tx_tag_get(skb);
}

static inline struct txbd8 *skip_txbd(struct txbd8 *bdp, int stride,
			       struct txbd8 *base, int ring_size)
{
	struct txbd8 *new_bd = bdp + stride;

	return (new_bd >= (base + ring_size)) ? (new_bd - ring_size) : new_bd;
}

static inline struct txbd8 *next_txbd(struct txbd8 *bdp, struct txbd8 *base,
		int ring_size)
{
	return skip_txbd(bdp, 1, base, ring_size);
}

/*software TCP segmentation offload*/
static int gfar_tso(struct sk_buff *skb, struct net_device *dev, int rq)
{
	int i = 0;
	struct iphdr *iph;
	int ihl;
	int id;
	unsigned int offset;
	struct tcphdr *th;
	unsigned thlen;
	unsigned int seq;
	u32 delta;
	u16 oldlen;
	unsigned int mss;
	unsigned int doffset;
	unsigned int headroom;
	unsigned int len;
	int nfrags;
	int pos;
	int hsize;
	int ret;

	/*processing mac header*/
	skb_reset_mac_header(skb);
	skb->mac_len = skb->network_header - skb->mac_header;
	__skb_pull(skb, skb->mac_len);

	/*processing IP header*/
	iph = ip_hdr(skb);
	ihl = iph->ihl * 4;
	id = ntohs(iph->id);
	__skb_pull(skb, ihl);

	/*processing TCP header*/
	skb_reset_transport_header(skb);
	th = tcp_hdr(skb);
	thlen = th->doff * 4;
	oldlen = ~skb->len;
	__skb_pull(skb, thlen);

	mss = skb_shinfo(skb)->gso_size;
	seq = ntohl(th->seq);
	delta = oldlen + (thlen + mss);

	/*processing SKB*/
	doffset = skb->data - skb_mac_header(skb);
	offset = doffset;
	nfrags = skb_shinfo(skb)->nr_frags;
	__skb_push(skb, doffset);
	headroom = skb_headroom(skb);
	pos = skb_headlen(skb);

	/*segmenting SKB*/
	hsize = skb_headlen(skb) - offset;
	if (hsize < 0)
		hsize = 0;

	do {
		struct sk_buff *nskb;
		skb_frag_t *frag;
		int size;

		len = skb->len - offset;
		if (len > mss)
			len = mss;

		nskb = gfar_new_skb(dev);
		nskb->dev = dev;
		skb_reserve(nskb, headroom);
		__skb_put(nskb, doffset+hsize);

		nskb->ip_summed = skb->ip_summed;
		nskb->vlan_tci = skb->vlan_tci;
		nskb->mac_len = skb->mac_len;

		skb_reset_mac_header(nskb);
		skb_set_network_header(nskb, skb->mac_len);
		nskb->transport_header = (nskb->network_header +
				skb_network_header_len(skb));

		/* Copy contiguous data which includes only the protocol
		 * headers.This is true when TSO is enabled,
		 * as data is carried by page */
		skb_copy_from_linear_data(skb, nskb->data, doffset+hsize);
		frag = skb_shinfo(nskb)->frags;

		/*move skb data from skb fragments to new skb*/
		while (pos < offset + len && i < nfrags) {
			*frag = skb_shinfo(skb)->frags[i];
			get_page(frag->page);
			size = frag->size;

			if (pos < offset) {
				frag->page_offset += offset - pos;
				frag->size -= offset - pos;
			}

			skb_shinfo(nskb)->nr_frags++;

			if (pos + size <= offset + len) {
				i++;
				pos += size;
			} else {
				frag->size -= pos + size - (offset + len);
				goto skip_fraglist;
			}

			frag++;
		}

skip_fraglist:
		nskb->data_len = len - hsize;
		/* Do not update nskb->truesize with size of fragments.
		 * Original value of truesize will be used on TX cleanup
		 * to identify this nskb as recyclable */
		nskb->len += nskb->data_len;

		/*update TCP header*/
		if ((offset + len) >= skb->len)
			delta = oldlen + (nskb->tail - nskb->transport_header) +
					nskb->data_len;

		th = tcp_hdr(nskb);
		th->fin = th->psh = 0;
		th->seq = htonl(seq);
		th->cwr = 0;
		seq += mss;
		th->check = ~csum_fold((__force __wsum)((__force u32)th->check
					+ delta));

		/*update IP header*/
		iph = ip_hdr(nskb);
		iph->id = htons(id++);
		iph->tot_len = htons(nskb->len - nskb->mac_len);
		iph->check = 0;
		iph->check = ip_fast_csum(skb_network_header(nskb), iph->ihl);

		ret = gfar_start_xmit(nskb, dev);
		if (unlikely(ret)) {
			dev_kfree_skb_any(nskb);
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
	} while ((offset += len) < skb->len);

	gfar_free_skb(skb);
	return ret;
}

static void gfar_align_skb(struct sk_buff *skb)
{
	/* We need the data buffer to be aligned properly.  We will reserve
	 * as many bytes as needed to align the data properly
	 */
	skb_reserve(skb, RXBUF_ALIGNMENT -
		(((unsigned long) skb->data) & (RXBUF_ALIGNMENT - 1)));
}

#ifdef CONFIG_AS_FASTPATH
static inline void gfar_asf_reclaim_skb(struct sk_buff *skb)
{
	/* Just reset the fields used in software DPA */
	skb->next = skb->prev = NULL;
	skb->dev = NULL;
	skb->len = 0;
	skb->ip_summed = 0;
	skb->transport_header = NULL;
	skb->mac_header = NULL;
	skb->network_header = NULL;
	skb->pkt_type = 0;
	skb->mac_len = 0;
	skb->protocol = 0;
	skb->vlan_tci = 0;
	skb->data = 0;

	/* reset data and tail pointers */
	skb->data = skb->head + NET_SKB_PAD;
	skb_reset_tail_pointer(skb);

}
#endif

/* This is called by the kernel when a frame is ready for transmission. */
/* It is pointed to by the dev->hard_start_xmit function pointer */
static int gfar_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar_priv_tx_q *tx_queue = NULL;
	struct netdev_queue *txq;
	struct gfar __iomem *regs = NULL;
	struct txfcb *fcb = NULL;
	struct txbd8 *txbdp, *txbdp_start, *base, *txbdp_tstamp = NULL;
	u32 lstatus;
	int i, rq = 0, do_tstamp = 0;
	u32 bufaddr;
	unsigned int nr_frags, nr_txbds, length;
#ifdef CONFIG_RX_TX_BUFF_XCHG
	struct sk_buff *new_skb;
	int skb_curtx = 0;
#else
	unsigned long flags;
#endif

#ifdef CONFIG_AS_FASTPATH
	if (devfp_tx_hook && (skb->pkt_type != PACKET_FASTROUTE))
		if (devfp_tx_hook(skb, dev) == AS_FP_STOLEN)
			return 0;
#endif
	/*
	 * TOE=1 frames larger than 2500 bytes may see excess delays
	 * before start of transmission.
	 */
	if (unlikely(gfar_has_errata(priv, GFAR_ERRATA_76) &&
			skb->ip_summed == CHECKSUM_PARTIAL &&
			skb->len > 2500)) {
		int ret;

		ret = skb_checksum_help(skb);
		if (ret)
			return ret;
	}

#ifdef CONFIG_RX_TX_BUFF_XCHG
	rq = smp_processor_id() + 1;
#else
	rq = skb->queue_mapping;
#endif
	tx_queue = priv->tx_queue[rq];
	txq = netdev_get_tx_queue(dev, rq);
	base = tx_queue->tx_bd_base;
	regs = tx_queue->grp->regs;

	/* check if time stamp should be generated */
	if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP &&
		     priv->hwts_tx_en) || unlikely(priv->hwts_tx_en_ioctl))
		do_tstamp = 1;

	/* make space for additional header when fcb is needed */
	if (((skb->ip_summed == CHECKSUM_PARTIAL) ||
			vlan_tx_tag_present(skb) ||
			unlikely(do_tstamp)) &&
			(skb_headroom(skb) < GMAC_FCB_LEN)) {
		struct sk_buff *skb_new;

		skb_new = skb_realloc_headroom(skb, GMAC_FCB_LEN);
		if (!skb_new) {
			dev->stats.tx_errors++;
			kfree_skb(skb);
			return NETDEV_TX_OK;
		}
		kfree_skb(skb);
		skb = skb_new;
	}

	if (skb_is_gso(skb) && !do_tstamp)
		return gfar_tso(skb, dev, rq);

	/* total number of fragments in the SKB */
	nr_frags = skb_shinfo(skb)->nr_frags;

	/* calculate the required number of TxBDs for this skb */
	if (unlikely(do_tstamp))
		nr_txbds = nr_frags + 2;
	else
		nr_txbds = nr_frags + 1;

#ifndef CONFIG_RX_TX_BUFF_XCHG
	/* check if there is space to queue this packet */
	if (nr_txbds > tx_queue->num_txbdfree) {
		/* no space, stop the queue */
		netif_tx_stop_queue(txq);
		dev->stats.tx_fifo_errors++;
		return NETDEV_TX_BUSY;
	}
#else
	txbdp = tx_queue->cur_tx;
	skb_curtx = tx_queue->skb_curtx;
	do {
		lstatus = txbdp->lstatus;
		if ((lstatus & BD_LFLAG(TXBD_READY))) {
			u32 imask;
			/* BD not free for tx */
			netif_tx_stop_queue(txq);
			dev->stats.tx_fifo_errors++;
			spin_lock_irq(&tx_queue->grp->grplock);
			imask = gfar_read(&regs->imask);
			imask |= IMASK_DEFAULT_TX;
			gfar_write(&regs->imask, imask);
			spin_unlock_irq(&tx_queue->grp->grplock);
			return NETDEV_TX_BUSY;
		}

		/* BD is free to be used by s/w */
		/* Free skb for this BD if not recycled */
		if (tx_queue->tx_skbuff[skb_curtx] &&
		    tx_queue->tx_skbuff[skb_curtx]->owner == KER_PKT_ID) {
			dev_kfree_skb_any(tx_queue->tx_skbuff[skb_curtx]);
			tx_queue->tx_skbuff[skb_curtx] = NULL;
		}

		txbdp->lstatus &= BD_LFLAG(TXBD_WRAP);
		skb_curtx = (skb_curtx + 1) & TX_RING_MOD_MASK(tx_queue->tx_ring_size);
		nr_txbds--;

		if (!nr_txbds)
			break;

		txbdp = next_txbd(txbdp, base, tx_queue->tx_ring_size);
	} while (1);
#endif
	/* Update transmit stats */
	tx_queue->stats.tx_bytes += skb->len;
	tx_queue->stats.tx_packets++;

	txbdp = txbdp_start = tx_queue->cur_tx;
	lstatus = txbdp->lstatus;

	/* Time stamp insertion requires one additional TxBD */
	if (unlikely(do_tstamp))
		txbdp_tstamp = txbdp = next_txbd(txbdp, base,
				tx_queue->tx_ring_size);

	if (nr_frags == 0) {
		if (unlikely(do_tstamp))
			txbdp_tstamp->lstatus |= BD_LFLAG(TXBD_LAST |
					TXBD_INTERRUPT);
		else
			lstatus |= BD_LFLAG(TXBD_LAST | TXBD_INTERRUPT);
	} else {
		/* Place the fragment addresses and lengths into the TxBDs */
		for (i = 0; i < nr_frags; i++) {
			/* Point at the next BD, wrapping as needed */
			txbdp = next_txbd(txbdp, base, tx_queue->tx_ring_size);

			length = skb_shinfo(skb)->frags[i].size;

			lstatus = txbdp->lstatus | length |
				BD_LFLAG(TXBD_READY);

			/* Handle the last BD specially */
			if (i == nr_frags - 1)
				lstatus |= BD_LFLAG(TXBD_LAST | TXBD_INTERRUPT);

			bufaddr = dma_map_page(&priv->ofdev->dev,
					skb_shinfo(skb)->frags[i].page,
					skb_shinfo(skb)->frags[i].page_offset,
					length,
					DMA_TO_DEVICE);

			/* set the TxBD length and buffer pointer */
			txbdp->bufPtr = bufaddr;
			txbdp->lstatus = lstatus;
		}

		lstatus = txbdp_start->lstatus;
	}

	/* Set up checksumming */
	if (CHECKSUM_PARTIAL == skb->ip_summed) {
		fcb = gfar_add_fcb(skb);
		/* as specified by errata */
		if (unlikely(gfar_has_errata(priv, GFAR_ERRATA_12)
			     && ((unsigned long)fcb % 0x20) > 0x18)) {
			__skb_pull(skb, GMAC_FCB_LEN);
			skb_checksum_help(skb);
		} else {
			lstatus |= BD_LFLAG(TXBD_TOE);
			gfar_tx_checksum(skb, fcb);
		}
	}

	if (vlan_tx_tag_present(skb)) {
		if (unlikely(NULL == fcb)) {
			fcb = gfar_add_fcb(skb);
			lstatus |= BD_LFLAG(TXBD_TOE);
		}

		gfar_tx_vlan(skb, fcb);
	}

	/* Setup tx hardware time stamping if requested */
	if (unlikely(do_tstamp)) {
		u32 vlan_ctrl;
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
		if (fcb == NULL)
			fcb = gfar_add_fcb(skb);
		/*
		 * the timestamp overwrites the ethertype and the following
		 * 2 bytes -> storing 4 bytes at the end of the control buffer
		 * structure - will be recovered in the function
		 * gfar_clean_tx_ring
		 */
		memcpy(skb->cb, (skb->data + GMAC_FCB_LEN +
					ETH_ALEN + ETH_ALEN), 4);
		fcb->ptp = 1;
		lstatus |= BD_LFLAG(TXBD_TOE);
		/*
		 * SYMM: When PTP in FCB is enabled, VLN in FCB is ignored.
		 * Instead VLAN tag is read from DFVLAN register. Thus need
		 * to copy VLCTL to DFVLAN register.
		 */
		vlan_ctrl = gfar_read(&regs->dfvlan);
		vlan_ctrl &= ~0xFFFF;
		vlan_ctrl |= (fcb->vlctl & 0xFFFF);
		gfar_write(&regs->dfvlan, vlan_ctrl);
	}

#ifdef CONFIG_RX_TX_BUFF_XCHG
	new_skb = tx_queue->tx_skbuff[tx_queue->skb_curtx];
	skb_curtx = tx_queue->skb_curtx;
	if (new_skb && skb->owner != RT_PKT_ID) {
		/* Packet from Kernel free the skb to recycle poll */
		new_skb->dev = dev;
		gfar_free_skb(new_skb);
		new_skb = NULL;
	}
#endif

	txbdp_start->bufPtr = dma_map_single(&priv->ofdev->dev, skb->data,
			skb_headlen(skb), DMA_TO_DEVICE);

	/*
	 * If time stamping is requested one additional TxBD must be set up. The
	 * first TxBD points to the FCB and must have a data length of
	 * GMAC_FCB_LEN. The second TxBD points to the actual frame data with
	 * the full frame length.
	 */
	if (unlikely(do_tstamp)) {
		txbdp_tstamp->bufPtr = txbdp_start->bufPtr + GMAC_FCB_LEN;
		txbdp_tstamp->lstatus |= BD_LFLAG(TXBD_READY) |
				(skb_headlen(skb) - GMAC_FCB_LEN);
		lstatus |= BD_LFLAG(TXBD_CRC | TXBD_READY) | GMAC_FCB_LEN;
	} else {
		lstatus |= BD_LFLAG(TXBD_CRC | TXBD_READY) | skb_headlen(skb);
	}

	/*
	 * We can work in parallel with gfar_clean_tx_ring(), except
	 * when modifying num_txbdfree. Note that we didn't grab the lock
	 * when we were reading the num_txbdfree and checking for available
	 * space, that's because outside of this function it can only grow,
	 * and once we've got needed space, it cannot suddenly disappear.
	 *
	 * The lock also protects us from gfar_error(), which can modify
	 * regs->tstat and thus retrigger the transfers, which is why we
	 * also must grab the lock before setting ready bit for the first
	 * to be transmitted BD.
	 */
#ifndef CONFIG_RX_TX_BUFF_XCHG
	spin_lock_irqsave(&tx_queue->txlock, flags);
#endif

	/*
	 * The powerpc-specific eieio() is used, as wmb() has too strong
	 * semantics (it requires synchronization between cacheable and
	 * uncacheable mappings, which eieio doesn't provide and which we
	 * don't need), thus requiring a more expensive sync instruction.  At
	 * some point, the set of architecture-independent barrier functions
	 * should be expanded to include weaker barriers.
	 */
	eieio();

	txbdp_start->lstatus = lstatus;

	eieio(); /* force lstatus write before tx_skbuff */

	tx_queue->tx_skbuff[tx_queue->skb_curtx] = skb;

	/* Update the current skb pointer to the next entry we will use
	 * (wrapping if necessary) */
	tx_queue->skb_curtx = (tx_queue->skb_curtx + 1) &
		TX_RING_MOD_MASK(tx_queue->tx_ring_size);

	tx_queue->cur_tx = next_txbd(txbdp, base, tx_queue->tx_ring_size);

#ifndef CONFIG_RX_TX_BUFF_XCHG
	/* reduce TxBD free count */
	tx_queue->num_txbdfree -= (nr_txbds);

	/* If the next BD still needs to be cleaned up, then the bds
	   are full.  We need to tell the kernel to stop sending us stuff. */
	if (!tx_queue->num_txbdfree) {
		netif_tx_stop_queue(txq);

		dev->stats.tx_fifo_errors++;
	}
#endif

	/* Tell the DMA to go go go */
	gfar_write(&regs->tstat, TSTAT_CLEAR_THALT >> tx_queue->qindex);

#ifndef CONFIG_RX_TX_BUFF_XCHG
	/* Unlock priv */
	spin_unlock_irqrestore(&tx_queue->txlock, flags);
#endif
#ifdef CONFIG_RX_TX_BUFF_XCHG
{
	struct net_device *dev = skb->dev;
	struct gfar_private *priv = netdev_priv(dev);

	if (!skb_is_recycleable(skb, priv->rx_buffer_size + RXBUF_ALIGNMENT))
		skb->owner = KER_PKT_ID;
	else {
#ifdef CONFIG_AS_FASTPATH
		if (skb->pkt_type == PACKET_FASTROUTE)
			gfar_asf_reclaim_skb(skb);
		else
#endif
			skb_recycle(skb);
		gfar_align_skb(skb);
	}
	skb->new_skb = new_skb;
	txq->trans_start = jiffies;
}
#endif

	return NETDEV_TX_OK;
}

#ifdef CONFIG_AS_FASTPATH
/*
 * This is function is called directly by ASF when ASF runs in Minimal mode
 * transmission.
 */
int gfar_fast_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar_priv_tx_q *tx_queue = NULL;
	struct netdev_queue *txq;
	struct gfar __iomem *regs = NULL;
	struct txbd8 *txbdp, *txbdp_start, *base;
	u32 lstatus;
	int rq = 0;
	unsigned long flags;

#ifdef CONFIG_RX_TX_BUFF_XCHG
	struct sk_buff *new_skb;
	int skb_curtx = 0;
#endif

#ifdef CONFIG_RX_TX_BUFF_XCHG
	rq = smp_processor_id() + 1;
#else
	rq = skb->queue_mapping;
#endif
	tx_queue = priv->tx_queue[rq];
	txq = netdev_get_tx_queue(dev, rq);
	base = tx_queue->tx_bd_base;
	regs = tx_queue->grp->regs;


#ifndef CONFIG_RX_TX_BUFF_XCHG
	/* check if there is space to queue this packet */
	if (unlikely(tx_queue->num_txbdfree < 1)) {
		/* no space, stop the queue */
		netif_tx_stop_queue(txq);
		dev->stats.tx_fifo_errors++;
		return NETDEV_TX_BUSY;
	}
#else
	txbdp = tx_queue->cur_tx;
	skb_curtx = tx_queue->skb_curtx;

	lstatus = txbdp->lstatus;
	if ((lstatus & BD_LFLAG(TXBD_READY))) {
		u32 imask;
		/* BD not free for tx */
		netif_tx_stop_queue(txq);
		dev->stats.tx_fifo_errors++;
		spin_lock_irq(&tx_queue->grp->grplock);
		imask = gfar_read(&regs->imask);
		imask |= IMASK_DEFAULT_TX;
		gfar_write(&regs->imask, imask);
		spin_unlock_irq(&tx_queue->grp->grplock);
		return NETDEV_TX_BUSY;
	}

	/* BD is free to be used by s/w */
	/* Free skb for this BD if not recycled */
	if (tx_queue->tx_skbuff[skb_curtx] &&
	    tx_queue->tx_skbuff[skb_curtx]->owner == KER_PKT_ID) {
		dev_kfree_skb_any(tx_queue->tx_skbuff[skb_curtx]);
		tx_queue->tx_skbuff[skb_curtx] = NULL;
	}

	txbdp->lstatus &= BD_LFLAG(TXBD_WRAP);
#endif

	/* Update transmit stats */
	tx_queue->stats.tx_bytes += skb->len;
	tx_queue->stats.tx_packets++;

	txbdp = txbdp_start = tx_queue->cur_tx;

	lstatus = txbdp->lstatus | BD_LFLAG(TXBD_LAST | TXBD_INTERRUPT);

	/* Set up checksumming */

	if (CHECKSUM_PARTIAL == skb->ip_summed) {
		struct txfcb *fcb = NULL;
		fcb = gfar_add_fcb(skb);
		lstatus |= BD_LFLAG(TXBD_TOE);
		gfar_tx_checksum(skb, fcb);
	}

#ifdef CONFIG_RX_TX_BUFF_XCHG
	new_skb = tx_queue->tx_skbuff[tx_queue->skb_curtx];
	skb_curtx =  tx_queue->skb_curtx;
	if (new_skb && (skb->owner != RT_PKT_ID)) {
		/* Packet from Kernel free the skb to recycle poll */
		new_skb->dev = dev;
		gfar_free_skb(new_skb);
		new_skb = NULL;
	}
#endif
	txbdp_start->bufPtr = dma_map_single(&priv->ofdev->dev, skb->data,
			skb_headlen(skb), DMA_TO_DEVICE);

	lstatus |= BD_LFLAG(TXBD_CRC | TXBD_READY) | skb_headlen(skb);
	/*
	 * We can work in parallel with gfar_clean_tx_ring(), except
	 * when modifying num_txbdfree. Note that we didn't grab the lock
	 * when we were reading the num_txbdfree and checking for available
	 * space, that's because outside of this function it can only grow,
	 * and once we've got needed space, it cannot suddenly disappear.
	 *
	 * The lock also protects us from gfar_error(), which can modify
	 * regs->tstat and thus retrigger the transfers, which is why we
	 * also must grab the lock before setting ready bit for the first
	 * to be transmitted BD.
	 */

#ifndef CONFIG_RX_TX_BUFF_XCHG
		spin_lock_irqsave(&tx_queue->txlock, flags);
#endif

	/*
	 * The powerpc-specific eieio() is used, as wmb() has too strong
	 * semantics (it requires synchronization between cacheable and
	 * uncacheable mappings, which eieio doesn't provide and which we
	 * don't need), thus requiring a more expensive sync instruction.  At
	 * some point, the set of architecture-independent barrier functions
	 * should be expanded to include weaker barriers.
	 */
	eieio();

	txbdp_start->lstatus = lstatus;

	eieio(); /* force lstatus write before tx_skbuff */

	/* setup the TxBD length and buffer pointer for the first BD */
	tx_queue->tx_skbuff[tx_queue->skb_curtx] = skb;

	/* Update the current skb pointer to the next entry we will use
	 * (wrapping if necessary) */
	tx_queue->skb_curtx = (tx_queue->skb_curtx + 1) &
		TX_RING_MOD_MASK(tx_queue->tx_ring_size);

	tx_queue->cur_tx = next_txbd(txbdp, base, tx_queue->tx_ring_size);

#ifndef CONFIG_RX_TX_BUFF_XCHG
	/* reduce TxBD free count */
	tx_queue->num_txbdfree -= 1;

	/* If the next BD still needs to be cleaned up, then the bds
	   are full.  We need to tell the kernel to stop sending us stuff. */
	if (unlikely(!tx_queue->num_txbdfree)) {
		netif_stop_subqueue(dev, tx_queue->qindex);
		dev->stats.tx_fifo_errors++;
	}
#endif

	/* Tell the DMA to go go go */
	gfar_write(&regs->tstat, TSTAT_CLEAR_THALT >> tx_queue->qindex);

#ifndef CONFIG_RX_TX_BUFF_XCHG
		spin_unlock_irqrestore(&tx_queue->txlock, flags);
#endif

#ifdef CONFIG_RX_TX_BUFF_XCHG
{
	struct net_device *dev = skb->dev;
	struct gfar_private *priv = netdev_priv(dev);

	if (!skb_is_recycleable(skb, priv->rx_buffer_size + RXBUF_ALIGNMENT))
		skb->owner = KER_PKT_ID;
	else {
		gfar_asf_reclaim_skb(skb);
		gfar_align_skb(skb);
	}
	skb->new_skb = new_skb;
	txq->trans_start = jiffies;
}
#endif
	return NETDEV_TX_OK;
}
EXPORT_SYMBOL(gfar_fast_xmit);
#endif

/* Stops the kernel queue, and halts the controller */
static int gfar_close(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);

	disable_napi(priv);

	cancel_work_sync(&priv->reset_task);
	stop_gfar(dev);

	/* Disconnect from the PHY */
	phy_disconnect(priv->phydev);
	priv->phydev = NULL;

	netif_tx_stop_all_queues(dev);

	return 0;
}

/* Changes the mac address if the controller is not running. */
static int gfar_set_mac_address(struct net_device *dev)
{
	gfar_set_mac_for_addr(dev, 0, dev->dev_addr);

	return 0;
}

/* Check if rx parser should be activated */
void gfar_check_rx_parser_mode(struct gfar_private *priv)
{
	struct gfar __iomem *regs;
	u32 tempval;

	regs = priv->gfargrp[0].regs;

	tempval = gfar_read(&regs->rctrl);
	/* If parse is no longer required, then disable parser */
	if (tempval & RCTRL_REQ_PARSER)
		tempval |= RCTRL_PRSDEP_INIT;
	else
		tempval &= ~RCTRL_PRSDEP_INIT;
	gfar_write(&regs->rctrl, tempval);
}

/* Enables and disables VLAN insertion/extraction */
void gfar_vlan_mode(struct net_device *dev, u32 features)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = NULL;
	unsigned long flags;
	u32 tempval;

	regs = priv->gfargrp[0].regs;
	local_irq_save(flags);
	lock_rx_qs(priv);

	if (features & NETIF_F_HW_VLAN_TX) {
		/* Enable VLAN tag insertion */
		tempval = gfar_read(&regs->tctrl);
		tempval |= TCTRL_VLINS;
		gfar_write(&regs->tctrl, tempval);
	} else {
		/* Disable VLAN tag insertion */
		tempval = gfar_read(&regs->tctrl);
		tempval &= ~TCTRL_VLINS;
		gfar_write(&regs->tctrl, tempval);
	}

	if (features & NETIF_F_HW_VLAN_RX) {
		/* Enable VLAN tag extraction */
		tempval = gfar_read(&regs->rctrl);
		tempval |= (RCTRL_VLEX | RCTRL_PRSDEP_INIT);
		gfar_write(&regs->rctrl, tempval);
	} else {
		/* Disable VLAN tag extraction */
		tempval = gfar_read(&regs->rctrl);
		tempval &= ~RCTRL_VLEX;
		gfar_write(&regs->rctrl, tempval);

		gfar_check_rx_parser_mode(priv);
	}

	gfar_change_mtu(dev, dev->mtu);

	unlock_rx_qs(priv);
	local_irq_restore(flags);
}

static int gfar_change_mtu(struct net_device *dev, int new_mtu)
{
	int tempsize, tempval;
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	int oldsize = priv->rx_buffer_size;
	int frame_size = new_mtu + ETH_HLEN;

	if (gfar_is_vlan_on(priv))
		frame_size += VLAN_HLEN;

	if ((frame_size < 64) || (frame_size > JUMBO_FRAME_SIZE)) {
		netif_err(priv, drv, dev, "Invalid MTU setting\n");
		return -EINVAL;
	}

	if (gfar_uses_fcb(priv))
		frame_size += GMAC_FCB_LEN;

	frame_size += priv->padding;

	tempsize =
	    (frame_size & ~(INCREMENTAL_BUFFER_SIZE - 1)) +
	    INCREMENTAL_BUFFER_SIZE;

	/* Only stop and start the controller if it isn't already
	 * stopped, and we changed something */
	if ((oldsize != tempsize) && (dev->flags & IFF_UP))
		stop_gfar(dev);

	priv->rx_buffer_size = tempsize;

	dev->mtu = new_mtu;

	gfar_write(&regs->mrblr, priv->rx_buffer_size);
	gfar_write(&regs->maxfrm, priv->rx_buffer_size);

	/* If the mtu is larger than the max size for standard
	 * ethernet frames (ie, a jumbo frame), then set maccfg2
	 * to allow huge frames, and to check the length */
	tempval = gfar_read(&regs->maccfg2);

	if (priv->rx_buffer_size > DEFAULT_RX_BUFFER_SIZE ||
			gfar_has_errata(priv, GFAR_ERRATA_74))
		tempval |= (MACCFG2_HUGEFRAME | MACCFG2_LENGTHCHECK);
	else
		tempval &= ~(MACCFG2_HUGEFRAME | MACCFG2_LENGTHCHECK);

	gfar_write(&regs->maccfg2, tempval);

	if ((oldsize != tempsize) && (dev->flags & IFF_UP))
		startup_gfar(dev);

	return 0;
}

/* gfar_reset_task gets scheduled when a packet has not been
 * transmitted after a set amount of time.
 * For now, assume that clearing out all the structures, and
 * starting over will fix the problem.
 */
static void gfar_reset_task(struct work_struct *work)
{
	struct gfar_private *priv = container_of(work, struct gfar_private,
			reset_task);
	struct net_device *dev = priv->ndev;

	if (dev->flags & IFF_UP) {
		netif_tx_stop_all_queues(dev);
		stop_gfar(dev);
		startup_gfar(dev);
		netif_tx_start_all_queues(dev);
	}

	netif_tx_schedule_all(dev);
}

static void gfar_timeout(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);

	dev->stats.tx_errors++;
	schedule_work(&priv->reset_task);
}

static int gfar_clean_tx_ring(struct gfar_priv_tx_q *tx_queue,
		int tx_work_limit)
{
	struct net_device *dev = tx_queue->dev;
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar_priv_rx_q *rx_queue = NULL;
	struct txbd8 *bdp, *next = NULL;
	struct txbd8 *lbdp = NULL;
	struct txbd8 *base = tx_queue->tx_bd_base;
	struct sk_buff *skb;
	int skb_dirtytx;
	int tx_ring_size = tx_queue->tx_ring_size;
	int frags = 0, nr_txbds = 0;
	int i;
	int howmany = 0;
	u32 lstatus;
	size_t buflen;

	rx_queue = priv->rx_queue[tx_queue->qindex];
	bdp = tx_queue->dirty_tx;
	skb_dirtytx = tx_queue->skb_dirtytx;

	while ((skb = tx_queue->tx_skbuff[skb_dirtytx]) && (tx_work_limit--)) {
		unsigned long flags;

		frags = skb_shinfo(skb)->nr_frags;

		/*
		 * When time stamping, one additional TxBD must be freed.
		 * Also, we need to dma_unmap_single() the TxPAL.
		 */
		if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS))
			nr_txbds = frags + 2;
		else
			nr_txbds = frags + 1;

		lbdp = skip_txbd(bdp, nr_txbds - 1, base, tx_ring_size);

		lstatus = lbdp->lstatus;

		/* Only clean completed frames */
		if ((lstatus & BD_LFLAG(TXBD_READY)) &&
				(lstatus & BD_LENGTH_MASK))
			break;

		if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS)) {
			next = next_txbd(bdp, base, tx_ring_size);
			buflen = next->length + GMAC_FCB_LEN;
		} else
			buflen = bdp->length;

		dma_unmap_single(&priv->ofdev->dev, bdp->bufPtr,
				buflen, DMA_TO_DEVICE);

		if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS)) {
			struct gfar __iomem *regs = priv->gfargrp[0].regs;
			struct skb_shared_hwtstamps shhwtstamps;
			u32 high, low;
			struct gfar_ptp_time tx_ts;
			u64 ns;

			if (priv->device_flags &
					FSL_GIANFAR_DEV_HAS_TS_TO_BUFFER) {
				/* get tx timestamp out of frame */
				void *ts;
				ts = (void *)(((uintptr_t)skb->data + 0x10)
						& ~0x7);
				ns = be64_to_cpup(ts);
			} else
				/* get tx timestamp from register */
				ns = gfar_get_tx_timestamp(regs);

			if (unlikely(priv->hwts_tx_en))
				shhwtstamps.hwtstamp = ns_to_ktime(ns);
			if (likely(priv->hwts_tx_en_ioctl)) {
				high = upper_32_bits(ns);
				low = lower_32_bits(ns);
				gfar_cnt_to_ptp_time(high, low, &tx_ts);
			}
			/* remove tx fcb */
			skb_pull(skb, GMAC_FCB_LEN);
			/*
			 * the timestamp overwrote the ethertype and the
			 * following 2 bytes, 4 byters were stored in the
			 * end of the control buffer in function
			 * gfar_start_xmit to be recovered here
			 */
			memcpy((skb->data + ETH_ALEN + ETH_ALEN), skb->cb, 4);
			/* pass timestamp back */
			if (unlikely(priv->hwts_tx_en))
				skb_tstamp_tx(skb, &shhwtstamps);
			if (likely(priv->hwts_tx_en_ioctl))
				gfar_ptp_store_txstamp(dev, skb, &tx_ts);
			bdp->lstatus &= BD_LFLAG(TXBD_WRAP);
			bdp = next;
		}

		bdp->lstatus &= BD_LFLAG(TXBD_WRAP);
		bdp = next_txbd(bdp, base, tx_ring_size);

		for (i = 0; i < frags; i++) {
			dma_unmap_page(&priv->ofdev->dev,
					bdp->bufPtr,
					bdp->length,
					DMA_TO_DEVICE);
			bdp->lstatus &= BD_LFLAG(TXBD_WRAP);
			bdp = next_txbd(bdp, base, tx_ring_size);
		}

		if (!skb_tcp_ack_recycle(skb))
			gfar_free_skb(skb);
		tx_queue->tx_skbuff[skb_dirtytx] = NULL;

		skb_dirtytx = (skb_dirtytx + 1) &
			TX_RING_MOD_MASK(tx_ring_size);

		howmany++;
		spin_lock_irqsave(&tx_queue->txlock, flags);
		tx_queue->num_txbdfree += nr_txbds;
		spin_unlock_irqrestore(&tx_queue->txlock, flags);
	}

	/* If we freed a buffer, we can restart transmission, if necessary */
	if (__netif_subqueue_stopped(dev, tx_queue->qindex) && tx_queue->num_txbdfree)
		netif_wake_subqueue(dev, tx_queue->qindex);

	/* Update dirty indicators */
	tx_queue->skb_dirtytx = skb_dirtytx;
	tx_queue->dirty_tx = bdp;

	return howmany;
}

static void gfar_schedule_rx_cleanup(struct gfar_priv_grp *gfargrp)
{
	unsigned long flags;
	u32 imask = 0;

	if (napi_schedule_prep(&gfargrp->napi_rx)) {
		spin_lock_irqsave(&gfargrp->grplock, flags);
		imask = gfar_read(&gfargrp->regs->imask);
		imask = imask & IMASK_RX_DISABLED;
		gfar_write(&gfargrp->regs->imask, imask);
		__napi_schedule(&gfargrp->napi_rx);
		spin_unlock_irqrestore(&gfargrp->grplock, flags);
	} else {
		gfar_write(&gfargrp->regs->ievent, IEVENT_RX_MASK);
	}
}

static void gfar_schedule_tx_cleanup(struct gfar_priv_grp *gfargrp)
{
	unsigned long flags;
	u32 imask = 0;

	if (napi_schedule_prep(&gfargrp->napi_tx)) {
		spin_lock_irqsave(&gfargrp->grplock, flags);
		imask = gfar_read(&gfargrp->regs->imask);
		imask = imask & IMASK_TX_DISABLED;
		gfar_write(&gfargrp->regs->imask, imask);
		__napi_schedule(&gfargrp->napi_tx);
		spin_unlock_irqrestore(&gfargrp->grplock, flags);
	} else {
		gfar_write(&gfargrp->regs->ievent, IEVENT_TX_MASK);
	}
}

/* Interrupt Handler for Transmit complete when TX NAPI mode is used*/
static irqreturn_t gfar_transmit(int irq, void *grp_id)
{
	gfar_schedule_tx_cleanup((struct gfar_priv_grp *)grp_id);
	return IRQ_HANDLED;
}

#ifdef CONFIG_RX_TX_BUFF_XCHG
static irqreturn_t gfar_enable_tx_queue(int irq, void *grp_id)
{
	struct gfar_priv_grp *grp = (struct gfar_priv_grp *)grp_id;
	struct gfar_private *priv = priv = grp->priv;
	struct gfar_priv_tx_q *tx_queue = NULL;
	u32 tstat, mask;
	int i;
	unsigned long flags;

	struct net_device *dev = NULL;
	tstat = gfar_read(&grp->regs->tstat);
	tstat = tstat & TSTAT_TXF_MASK_ALL;

	/* Clear IEVENT */
	gfar_write(&grp->regs->ievent, IEVENT_TX_MASK);

	for_each_set_bit(i, &grp->tx_bit_map, priv->num_tx_queues) {
		mask = TSTAT_TXF0_MASK >> i;
		if (tstat & mask) {
			tx_queue = priv->tx_queue[i];
			dev = tx_queue->dev;
			if (__netif_subqueue_stopped(dev, tx_queue->qindex))
				netif_wake_subqueue(dev, tx_queue->qindex);
		}
	}

	spin_lock_irqsave(&grp->grplock, flags);
	mask = gfar_read(&grp->regs->imask);
	mask = mask & IMASK_TX_DISABLED;
	gfar_write(&grp->regs->imask, mask);
	spin_unlock_irqrestore(&grp->grplock, flags);

	return IRQ_HANDLED;
}
#endif

/* Interrupt Handler for Transmit complete when TX NO NAPI mode is used*/
#ifndef CONFIG_RX_TX_BUFF_XCHG
static irqreturn_t gfar_transmit_no_napi(int irq, void *grp_id)
{
	struct gfar_priv_grp *grp = (struct gfar_priv_grp *)grp_id;
	struct gfar_private *priv = priv = grp->priv;
	struct gfar_priv_tx_q *tx_queue = NULL;
	u32 tstat, mask;
	int i;

	tstat = gfar_read(&grp->regs->tstat);
	tstat = tstat & TSTAT_TXF_MASK_ALL;

	/* Clear IEVENT */
	gfar_write(&grp->regs->ievent, IEVENT_TX_MASK);

	for_each_set_bit(i, &grp->tx_bit_map, priv->num_tx_queues) {
		mask = TSTAT_TXF0_MASK >> i;
		if (tstat & mask) {
			tx_queue = priv->tx_queue[i];
			/* Use the same cleanup function for both NAPI and
			 * No-NAPI modes. For No-NAPI configure the budget
			 * to a big enough value to be sure the cleanup
			 * function will not exit because budget is met.
			*/
			gfar_clean_tx_ring(tx_queue, GFAR_TX_MAX_RING_SIZE);
		}
	}

	gfar_configure_tx_coalescing(priv, grp->tx_bit_map);
	return IRQ_HANDLED;
}
#endif

static void gfar_new_rxbdp(struct gfar_priv_rx_q *rx_queue, struct rxbd8 *bdp,
		struct sk_buff *skb)
{
	struct net_device *dev = rx_queue->dev;
	struct gfar_private *priv = netdev_priv(dev);
	dma_addr_t buf;

	buf = dma_map_single(&priv->ofdev->dev, skb->data,
			     priv->rx_buffer_size, DMA_FROM_DEVICE);
	gfar_init_rxbdp(rx_queue, bdp, buf);
}

static struct sk_buff * gfar_alloc_skb(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct sk_buff *skb = NULL;

	skb = netdev_alloc_skb(dev, priv->rx_buffer_size + RXBUF_ALIGNMENT);
	if (!skb)
		return NULL;

	gfar_align_skb(skb);

	return skb;
}

static inline bool gfar_skb_nonlinear_recycleable(struct sk_buff *skb,
		int skb_size)
{
	if (!skb_is_nonlinear(skb))
		return false;

	/* True size allocated for an skb */
	if (skb->truesize != SKB_DATA_ALIGN(skb_size + NET_SKB_PAD)
				+ sizeof(struct sk_buff))
		return false;

	return true;
}

void gfar_free_skb(struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;
	struct gfar_private *priv = netdev_priv(dev);
	struct sk_buff_head *recycle_q, *temp_recycle_q;
	struct gfar_recycle_cntxt *recycle_cntxt;
	struct gfar_recycle_cntxt_percpu *local;
	unsigned long flags;
	int cpu, skb_size;

	skb_size = priv->rx_buffer_size + RXBUF_ALIGNMENT;
	recycle_cntxt = priv->recycle;

	if (!skb_is_recycleable(skb, skb_size)) {
		if (!gfar_skb_nonlinear_recycleable(skb, skb_size)) {
			dev_kfree_skb_any(skb);
			return;
		}

		/*
		 * skb was alocated in driver, hence the size of
		 * contiguous buffer in skb is big enough to recycle it for rx.
		 * Clean first the SKB fragments and test again.
		 * Possible usecase is TSO, when driver allocates new skb and
		 * then it can add fragments to new skb. In this case,
		 * skb_is_recycleable() returns false because skb is not linear.
		 */
		if (skb_shinfo(skb)->nr_frags) {
			int i;
			for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
				put_page(skb_shinfo(skb)->frags[i].page);
			skb_shinfo(skb)->nr_frags = 0;
			skb->data_len = 0;
		}

		if (!skb_is_recycleable(skb, skb_size)) {
			dev_kfree_skb_any(skb);
			return;
		}
	}

	skb_recycle(skb);
	gfar_align_skb(skb);

	cpu = get_cpu();
	local = per_cpu_ptr(recycle_cntxt->local, cpu);
	recycle_q = local->recycle_q;

	if (skb_queue_len(recycle_q) < recycle_cntxt->recycle_max) {
		local->free_count++;
		__skb_queue_head(recycle_q, skb);
		put_cpu();
		return;
	}

	/* Local per CPU queue is full. Now swap this full recycle queue
	 * with global device recycle queue if it is empty otherwise
	 * kfree the skb
	 */
	spin_lock_irqsave(&recycle_cntxt->recycle_lock, flags);
	if (recycle_cntxt->global_recycle_q &&
		!skb_queue_len(recycle_cntxt->global_recycle_q)) {

		temp_recycle_q  = recycle_cntxt->global_recycle_q;
		recycle_cntxt->global_recycle_q = recycle_q;
		recycle_cntxt->free_swap_count++;
		spin_unlock_irqrestore(&recycle_cntxt->recycle_lock, flags);
		local->recycle_q = temp_recycle_q;
		local->free_count++;
		__skb_queue_head(temp_recycle_q, skb);
		put_cpu();
	} else {
		spin_unlock_irqrestore(&recycle_cntxt->recycle_lock, flags);
		put_cpu();
		dev_kfree_skb_any(skb);
	}
}
EXPORT_SYMBOL(gfar_free_skb);

struct sk_buff * gfar_new_skb(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct sk_buff *skb = NULL;
	struct sk_buff_head *recycle_q, *temp_recycle_q;
	struct gfar_recycle_cntxt *recycle_cntxt;
	struct gfar_recycle_cntxt_percpu *local;
	unsigned long flags;
	int cpu;

	recycle_cntxt = priv->recycle;

	cpu = get_cpu();
	local = per_cpu_ptr(recycle_cntxt->local, cpu);
	recycle_q = local->recycle_q;
	skb = __skb_dequeue(recycle_q);
	if (skb) {
		local->alloc_count++;
		put_cpu();
		return skb;
	}

	/* Local per cpu queue is empty. Now swap global recycle
	 * queue (if it is full) with this empty local queue.
	 */
	spin_lock_irqsave(&recycle_cntxt->recycle_lock, flags);
	if (recycle_cntxt->global_recycle_q &&
		skb_queue_len(recycle_cntxt->global_recycle_q)) {

		temp_recycle_q = recycle_cntxt->global_recycle_q;
		recycle_cntxt->global_recycle_q = recycle_q;
		recycle_cntxt->alloc_swap_count++;
		spin_unlock_irqrestore(&recycle_cntxt->recycle_lock, flags);
		local->recycle_q = temp_recycle_q;
		local->alloc_count++;
		skb = __skb_dequeue(temp_recycle_q);
		put_cpu();
	} else {
		spin_unlock_irqrestore(&recycle_cntxt->recycle_lock, flags);
		put_cpu();
		skb = gfar_alloc_skb(dev);
	}

	return skb;
}
EXPORT_SYMBOL(gfar_new_skb);

static inline void count_errors(unsigned short status, struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &dev->stats;
	struct gfar_extra_stats *estats = &priv->extra_stats;

	/* If the packet was truncated, none of the other errors
	 * matter */
	if (status & RXBD_TRUNCATED) {
		stats->rx_length_errors++;

		estats->rx_trunc++;

		return;
	}
	/* Count the errors, if there were any */
	if (status & (RXBD_LARGE | RXBD_SHORT)) {
		stats->rx_length_errors++;

		if (status & RXBD_LARGE)
			estats->rx_large++;
		else
			estats->rx_short++;
	}
	if (status & RXBD_NONOCTET) {
		stats->rx_frame_errors++;
		estats->rx_nonoctet++;
	}
	if (status & RXBD_CRCERR) {
		estats->rx_crcerr++;
		stats->rx_crc_errors++;
	}
	if (status & RXBD_OVERRUN) {
		estats->rx_overrun++;
		stats->rx_crc_errors++;
	}
}

irqreturn_t gfar_receive(int irq, void *grp_id)
{
	struct gfar_priv_grp *gfargrp = grp_id;
	struct gfar __iomem *regs = gfargrp->regs;
	u32 ievent;

	ievent = gfar_read(&regs->ievent);

	if ((ievent & IEVENT_FGPI) == IEVENT_FGPI) {
		gfar_write(&regs->ievent, ievent & IEVENT_RX_MASK);
		return IRQ_HANDLED;
	}

	gfar_schedule_rx_cleanup((struct gfar_priv_grp *)grp_id);
	return IRQ_HANDLED;
}

static inline void gfar_rx_checksum(struct sk_buff *skb, struct rxfcb *fcb)
{
	/* If valid headers were found, and valid sums
	 * were verified, then we tell the kernel that no
	 * checksumming is necessary.  Otherwise, it is */
	if ((fcb->flags & RXFCB_CSUM_MASK) == (RXFCB_CIP | RXFCB_CTU))
		skb->ip_summed = CHECKSUM_UNNECESSARY;
	else
		skb_checksum_none_assert(skb);
}


/* gfar_process_frame() -- handle one incoming packet if skb
 * isn't NULL.  */
static int gfar_process_frame(struct net_device *dev, struct sk_buff *skb,
			      int amount_pull)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct rxfcb *fcb = NULL;

	int ret;

	/* fcb is at the beginning if exists */
	fcb = (struct rxfcb *)skb->data;

	/* Remove the FCB from the skb */
	/* Remove the padded bytes, if there are any */
	if (amount_pull) {
		skb_record_rx_queue(skb, fcb->rq);
		skb_pull(skb, amount_pull);
	}

	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_TIMER) {
		u32 high, low;

		/* get timestamp */
		high = *((u32 *)skb->data);
		low = *(((u32 *)skb->data) + 1);
		skb_pull(skb, 8);
		/* proprietary PTP timestamping over ioctl */
		if (unlikely(priv->hwts_rx_en_ioctl)) {
			struct gfar_ptp_time rx_ts;
			/* get rx timestamp */
			gfar_cnt_to_ptp_time(high, low, &rx_ts);
			/* parse and store rx timestamp */
			gfar_ptp_store_rxstamp(dev, skb, &rx_ts);
		} else if (unlikely(priv->hwts_rx_en)) {
			/* kernel-API timestamping ? */
			u64 nsec;
			struct skb_shared_hwtstamps *hws;
			hws = skb_hwtstamps(skb);
			nsec = make64(high, low);
			hws->hwtstamp = ns_to_ktime(nsec);
		}
	} else if (priv->padding)
			skb_pull(skb, priv->padding);

	if (dev->features & NETIF_F_RXCSUM)
		gfar_rx_checksum(skb, fcb);

#ifdef CONFIG_AS_FASTPATH
	if (devfp_rx_hook) {
		/* Drop the packet silently if IP Checksum is not correct */
		if ((fcb->flags & RXFCB_CIP) && (fcb->flags & RXFCB_EIP)) {
			skb->dev = dev;
			gfar_free_skb(skb);
			return 0;
		}

		if (dev->features & NETIF_F_HW_VLAN_RX &&
		    fcb->flags & RXFCB_VLN)
			__vlan_hwaccel_put_tag(skb, fcb->vlctl);
		skb->dev = dev;

		if (devfp_rx_hook(skb, dev) == AS_FP_STOLEN)
			return 0;
	}
#endif
	/* Tell the skb what kind of packet this is */
	skb->protocol = eth_type_trans(skb, dev);

	/* There's need to check for NETIF_F_HW_VLAN_RX here.
	 * Even if vlan rx accel is disabled, on some chips
	 * RXFCB_VLN is pseudo randomly set.
	 */
	if (dev->features & NETIF_F_HW_VLAN_RX &&
	    fcb->flags & RXFCB_VLN)
		__vlan_hwaccel_put_tag(skb, fcb->vlctl);

	/* Send the packet up the stack */
	ret = netif_receive_skb(skb);

	if (NET_RX_DROP == ret)
		priv->extra_stats.kernel_dropped++;

	return 0;
}

#ifdef CONFIG_GFAR_HW_TCP_RECEIVE_OFFLOAD
static inline void gfar_hwaccel_tcp4_receive(struct gfar_private *priv,
					     struct gfar_priv_rx_q *rx_queue,
					     struct sk_buff *skb)
{
	const struct tcphdr *th;
	const struct iphdr *iph;
	int p_len;
	int ph_len;
	struct rxfcb *fcb;
	struct sock *gfar_sk;
	int tcp_chan_idx = rx_queue->qindex - GFAR_TCP_START_Q_IDX;

	/*
	 * mark this skb to be checked by the gfar hw tcp rcv setup code
	 * "hooked" inside tcp_v4_do_rcv()
	 */
	skb->gfar_dev = priv->ndev;
	if ((tcp_chan_idx < 0) || !priv->hw_tcp.chan[tcp_chan_idx]) {
		gfar_process_frame(priv->ndev, skb, GMAC_FCB_LEN);
		return;
	}

	gfar_sk = priv->hw_tcp.chan[tcp_chan_idx];

	fcb = (struct rxfcb *)skb->data;
	gfar_rx_checksum(skb, fcb);

	skb->pkt_type = PACKET_HOST;
	/* set IPv4 header */
	skb->network_header = skb->data + GMAC_FCB_LEN \
				+ ETH_HLEN + priv->padding;
	iph = ip_hdr(skb);

	if (iph->ihl > 5 || (iph->frag_off & htons(IP_MF | IP_OFFSET)) ||
		(gfar_sk->sk_state != TCP_ESTABLISHED)) {
		gfar_process_frame(priv->ndev, skb, GMAC_FCB_LEN);
		return;
	}

	ph_len = iph->ihl * 4; /* IPv4 header length, in bytes */
	p_len = ntohs(iph->tot_len); /* total length, in bytes */

	if (p_len <  (skb->len - GMAC_FCB_LEN - ETH_HLEN)) {
		skb->tail -= (skb->len - GMAC_FCB_LEN - ETH_HLEN - p_len);
		skb->len = p_len - ph_len;
	} else
		skb->len = skb->len - (GMAC_FCB_LEN + ETH_HLEN + ph_len);

	/*set TCP header*/
	skb->transport_header = skb->network_header + ph_len;
	skb->data = skb->transport_header;
	th = tcp_hdr(skb);
	TCP_SKB_CB(skb)->seq = ntohl(th->seq);
	TCP_SKB_CB(skb)->end_seq = (TCP_SKB_CB(skb)->seq + th->syn + th->fin +
					skb->len - (th->doff * 4));
	TCP_SKB_CB(skb)->ack_seq = ntohl(th->ack_seq);
	TCP_SKB_CB(skb)->when	 = 0;
	TCP_SKB_CB(skb)->flags	 = iph->tos;
	TCP_SKB_CB(skb)->sacked	 = 0;

	bh_lock_sock(gfar_sk);
	if (!sock_owned_by_user(gfar_sk)) {
		if (tcp_rcv_established(gfar_sk, skb, tcp_hdr(skb), skb->len)) {
			tcp_v4_send_reset(gfar_sk, skb);
			kfree_skb(skb);
		}
	} else
		sk_add_backlog(gfar_sk, skb);
	bh_unlock_sock(gfar_sk);
}
#endif

/* gfar_clean_rx_ring() -- Processes each frame in the rx ring
 *   until the budget/quota has been reached. Returns the number
 *   of frames handled
 */
int gfar_clean_rx_ring(struct gfar_priv_rx_q *rx_queue, int rx_work_limit)
{
	struct net_device *dev = rx_queue->dev;
	struct rxbd8 *bdp, *base;
	struct sk_buff *skb;
	int pkt_len;
	int amount_pull;
	int howmany = 0;
	struct gfar_private *priv = netdev_priv(dev);

	/* Get the first full descriptor */
	bdp = rx_queue->cur_rx;
	base = rx_queue->rx_bd_base;

	amount_pull = (gfar_uses_fcb(priv) ? GMAC_FCB_LEN : 0);

	while (!((bdp->status & RXBD_EMPTY) || (--rx_work_limit < 0))) {
		struct sk_buff *newskb = NULL;
		rmb();

#ifndef CONFIG_RX_TX_BUFF_XCHG
		/* Add another skb for the future */
		newskb = gfar_new_skb(dev);
#endif

		skb = rx_queue->rx_skbuff[rx_queue->skb_currx];

		dma_unmap_single(&priv->ofdev->dev, bdp->bufPtr,
				priv->rx_buffer_size, DMA_FROM_DEVICE);

		if (unlikely(!(bdp->status & RXBD_ERR) &&
				bdp->length > priv->rx_buffer_size))
			bdp->status = RXBD_LARGE;

#ifndef CONFIG_RX_TX_BUFF_XCHG
		/* We drop the frame if we failed to allocate a new buffer */
		if (unlikely(!newskb || !(bdp->status & RXBD_LAST) ||
				 bdp->status & RXBD_ERR)) {
			count_errors(bdp->status, dev);

			if (unlikely(!newskb))
				newskb = skb;
			else if (skb) {
				skb->dev = dev;
				gfar_free_skb(skb);
			}
#else
		if (unlikely(!(bdp->status & RXBD_LAST) ||
				bdp->status & RXBD_ERR)) {
			count_errors(bdp->status, dev);
			newskb = skb;
#endif
		} else {
			/* Increment the number of packets */
			rx_queue->stats.rx_packets++;
			howmany++;

			if (likely(skb)) {
				pkt_len = bdp->length - ETH_FCS_LEN;
				/* Remove the FCS from the packet length */
				skb_put(skb, pkt_len);
				rx_queue->stats.rx_bytes += pkt_len;
				skb_record_rx_queue(skb, rx_queue->qindex);
#ifdef CONFIG_RX_TX_BUFF_XCHG
				skb->owner = RT_PKT_ID;
#endif
#ifdef CONFIG_GFAR_HW_TCP_RECEIVE_OFFLOAD
				if (likely(priv->hw_tcp.en))
					gfar_hwaccel_tcp4_receive
							(priv, rx_queue, skb);
				else
#endif
				gfar_process_frame(dev, skb, amount_pull);
#ifdef CONFIG_RX_TX_BUFF_XCHG
				newskb = skb->new_skb;
				skb->owner = 0;
				skb->new_skb = NULL;
#endif

			} else {
				netif_warn(priv, rx_err, dev, "Missing skb!\n");
				rx_queue->stats.rx_dropped++;
				priv->extra_stats.rx_skbmissing++;
			}
		}

#ifdef CONFIG_RX_TX_BUFF_XCHG
		if (!newskb) {
			/* Allocate new skb for Rx ring */
			newskb = gfar_new_skb(dev);
		}

		if (!newskb)
			/* All memory Exhausted,a BUG */
			BUG();
#endif
		rx_queue->rx_skbuff[rx_queue->skb_currx] = newskb;

		/* Setup the new bdp */
		gfar_new_rxbdp(rx_queue, bdp, newskb);

		/* Update to the next pointer */
		bdp = next_bd(bdp, base, rx_queue->rx_ring_size);

		/* update to point at the next skb */
		rx_queue->skb_currx =
		    (rx_queue->skb_currx + 1) &
		    RX_RING_MOD_MASK(rx_queue->rx_ring_size);
	}

	/* Update the current rxbd pointer to be the next one */
	rx_queue->cur_rx = bdp;

	return howmany;
}

static int gfar_poll_rx(struct napi_struct *napi, int budget)
{
	struct gfar_priv_grp *gfargrp = container_of(napi,
			struct gfar_priv_grp, napi_rx);
	struct gfar_private *priv = gfargrp->priv;
	struct gfar __iomem *regs = gfargrp->regs;
	struct gfar_priv_rx_q *rx_queue = NULL;
	int rx_cleaned = 0, budget_per_queue = 0, rx_cleaned_per_queue = 0;
	int i, num_act_qs = 0, napi_done = 1;
	u32 imask, ievent, rstat, rstat_local, rstat_rxf, rstat_rhalt = 0, mask;

	rstat = gfar_read(&regs->rstat);
	rstat_rxf = (rstat & RSTAT_RXF_ALL_MASK);
	rstat_rxf |= gfargrp->rstat_prev;
	rstat_local = rstat_rxf;

	while (rstat_local) {
		num_act_qs++;
		rstat_local &= (rstat_local - 1);
	}
	budget_per_queue = budget/num_act_qs;

	gfar_write(&regs->rstat, rstat_rxf);
	gfar_write(&gfargrp->regs->ievent, IEVENT_RX_MASK);
	gfargrp->rstat_prev = rstat_rxf;

	for_each_set_bit(i, &gfargrp->rx_bit_map, priv->num_rx_queues) {
		mask = RSTAT_RXF0_MASK >> i;
		if (rstat_rxf & mask) {
			rx_queue = priv->rx_queue[i];
			rx_cleaned_per_queue = gfar_clean_rx_ring(rx_queue,
					budget_per_queue);
			if (rx_cleaned_per_queue >= budget_per_queue) {
				napi_done = 0;
			} else {
				gfargrp->rstat_prev &= ~(mask);
				rstat_rhalt |= RSTAT_CLEAR_RHALT >> i;
			}

			rx_cleaned += rx_cleaned_per_queue;
		}
	}

	if (rstat_rhalt)
		gfar_write(&regs->rstat, rstat_rhalt);


	if (napi_done) {
		napi_complete(napi);
		gfar_configure_rx_coalescing(priv, gfargrp->rx_bit_map);
		spin_lock_irq(&gfargrp->grplock);
		imask = gfar_read(&regs->imask);
		imask |= IMASK_DEFAULT_RX;
		gfar_write(&regs->imask, imask);
		ievent = gfar_read(&regs->ievent);
		ievent &= IEVENT_RX_MASK;
		if (ievent) {
			imask = imask & IMASK_RX_DISABLED;
			gfar_write(&gfargrp->regs->imask, imask);
			gfar_write(&gfargrp->regs->ievent, IEVENT_RX_MASK);
			napi_schedule(napi);
		}
		spin_unlock_irq(&gfargrp->grplock);
	}

	return rx_cleaned;
}

static int gfar_poll_tx(struct napi_struct *napi, int budget)
{
	struct gfar_priv_grp *gfargrp = container_of(napi,
			struct gfar_priv_grp, napi_tx);
	struct gfar_private *priv = gfargrp->priv;
	struct gfar __iomem *regs = gfargrp->regs;
	struct gfar_priv_tx_q *tx_queue = NULL;
	int tx_cleaned = 0, budget_per_queue = 0, tx_cleaned_per_queue = 0;
	int i, num_act_qs = 0, napi_done = 1;
	u32 imask, tstat, tstat_local, mask;


	tstat = gfar_read(&regs->tstat);
	tstat = tstat & TSTAT_TXF_MASK_ALL;
	tstat_local = tstat;

	while (tstat_local) {
		num_act_qs++;
		tstat_local &= (tstat_local - 1);
	}
	budget_per_queue = budget/num_act_qs;

	/* Clear IEVENT, so interrupts aren't called again
	 * because of the packets that have already arrived */
	gfar_write(&regs->ievent, IEVENT_TX_MASK);

	for_each_set_bit(i, &gfargrp->tx_bit_map, priv->num_tx_queues) {
		mask = TSTAT_TXF0_MASK >> i;
		if (tstat & mask) {
			tx_queue = priv->tx_queue[i];
			tx_cleaned_per_queue = gfar_clean_tx_ring(tx_queue,
					budget_per_queue);
			tx_cleaned += tx_cleaned_per_queue;
			napi_done &= (tx_cleaned_per_queue < budget_per_queue);
		}
	}

	if (napi_done) {
		napi_complete(napi);
		gfar_configure_tx_coalescing(priv, gfargrp->tx_bit_map);
		spin_lock_irq(&gfargrp->grplock);
		imask = gfar_read(&regs->imask);
		imask |= IMASK_DEFAULT_TX;
		gfar_write(&regs->imask, imask);
		spin_unlock_irq(&gfargrp->grplock);
	}

	return tx_cleaned;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
/*
 * Polling 'interrupt' - used by things like netconsole to send skbs
 * without having to re-enable interrupts. It's not called while
 * the interrupt routine is executing.
 */
static void gfar_netpoll(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	int i = 0;

	/* If the device has multiple interrupts, run tx/rx */
	if (priv->device_flags & FSL_GIANFAR_DEV_HAS_MULTI_INTR) {
		for (i = 0; i < priv->num_grps; i++) {
			disable_irq(priv->gfargrp[i].interruptTransmit);
			disable_irq(priv->gfargrp[i].interruptReceive);
			disable_irq(priv->gfargrp[i].interruptError);
			gfar_interrupt(priv->gfargrp[i].interruptTransmit,
						&priv->gfargrp[i]);
			enable_irq(priv->gfargrp[i].interruptError);
			enable_irq(priv->gfargrp[i].interruptReceive);
			enable_irq(priv->gfargrp[i].interruptTransmit);
		}
	} else {
		for (i = 0; i < priv->num_grps; i++) {
			disable_irq(priv->gfargrp[i].interruptTransmit);
			gfar_interrupt(priv->gfargrp[i].interruptTransmit,
						&priv->gfargrp[i]);
			enable_irq(priv->gfargrp[i].interruptTransmit);
		}
	}
}
#endif

/* The interrupt handler for devices with one interrupt */
static irqreturn_t gfar_interrupt(int irq, void *grp_id)
{
	struct gfar_priv_grp *gfargrp = grp_id;

	/* Save ievent for future reference */
	u32 events = gfar_read(&gfargrp->regs->ievent);

	/* Check for reception */
	if (events & IEVENT_RX_MASK)
		gfar_receive(irq, grp_id);

	/* Check for transmit completion */
	if (events & IEVENT_TX_MASK)
		gfar_transmit(irq, grp_id);

	/* Check for errors */
	if (events & IEVENT_ERR_MASK)
		gfar_error(irq, grp_id);

	return IRQ_HANDLED;
}

/* Called every time the controller might need to be made
 * aware of new link state.  The PHY code conveys this
 * information through variables in the phydev structure, and this
 * function converts those variables into the appropriate
 * register values, and can bring down the device if needed.
 */
static void adjust_link(struct net_device *dev)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	unsigned long flags;
	struct phy_device *phydev = priv->phydev;
	int new_state = 0;

	local_irq_save_nort(flags);
	lock_tx_qs(priv);

	if (phydev->link) {
		u32 tempval = gfar_read(&regs->maccfg2);
		u32 ecntrl = gfar_read(&regs->ecntrl);

		/* Now we make sure that we can be in full duplex mode.
		 * If not, we operate in half-duplex mode. */
		if (phydev->duplex != priv->oldduplex) {
			new_state = 1;
			if (!(phydev->duplex))
				tempval &= ~(MACCFG2_FULL_DUPLEX);
			else
				tempval |= MACCFG2_FULL_DUPLEX;

			priv->oldduplex = phydev->duplex;
		}

		if (phydev->speed != priv->oldspeed) {
			new_state = 1;
			switch (phydev->speed) {
			case 1000:
				tempval =
				    ((tempval & ~(MACCFG2_IF)) | MACCFG2_GMII);

				ecntrl &= ~(ECNTRL_R100);
				break;
			case 100:
			case 10:
				tempval =
				    ((tempval & ~(MACCFG2_IF)) | MACCFG2_MII);

				/* Reduced mode distinguishes
				 * between 10 and 100 */
				if (phydev->speed == SPEED_100)
					ecntrl |= ECNTRL_R100;
				else
					ecntrl &= ~(ECNTRL_R100);
				break;
			default:
				netif_warn(priv, link, dev,
					   "Ack!  Speed (%d) is not 10/100/1000!\n",
					   phydev->speed);
				break;
			}

			priv->oldspeed = phydev->speed;
		}

		gfar_write(&regs->maccfg2, tempval);
		gfar_write(&regs->ecntrl, ecntrl);

		if (!priv->oldlink) {
			new_state = 1;
			priv->oldlink = 1;
		}
	} else if (priv->oldlink) {
		new_state = 1;
		priv->oldlink = 0;
		priv->oldspeed = 0;
		priv->oldduplex = -1;
	}

	if (new_state && netif_msg_link(priv))
		phy_print_status(phydev);
	unlock_tx_qs(priv);
	local_irq_restore_nort(flags);
}

/* Update the hash table based on the current list of multicast
 * addresses we subscribe to.  Also, change the promiscuity of
 * the device based on the flags (this function is called
 * whenever dev->flags is changed */
static void gfar_set_multi(struct net_device *dev)
{
	struct netdev_hw_addr *ha;
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	u32 tempval;

	if (dev->flags & IFF_PROMISC) {
		/* Set RCTRL to PROM */
		tempval = gfar_read(&regs->rctrl);
		tempval |= RCTRL_PROM;
		gfar_write(&regs->rctrl, tempval);
	} else {
		/* Set RCTRL to not PROM */
		tempval = gfar_read(&regs->rctrl);
		tempval &= ~(RCTRL_PROM);
		gfar_write(&regs->rctrl, tempval);
	}

	if (dev->flags & IFF_ALLMULTI) {
		/* Set the hash to rx all multicast frames */
		gfar_write(&regs->igaddr0, 0xffffffff);
		gfar_write(&regs->igaddr1, 0xffffffff);
		gfar_write(&regs->igaddr2, 0xffffffff);
		gfar_write(&regs->igaddr3, 0xffffffff);
		gfar_write(&regs->igaddr4, 0xffffffff);
		gfar_write(&regs->igaddr5, 0xffffffff);
		gfar_write(&regs->igaddr6, 0xffffffff);
		gfar_write(&regs->igaddr7, 0xffffffff);
		gfar_write(&regs->gaddr0, 0xffffffff);
		gfar_write(&regs->gaddr1, 0xffffffff);
		gfar_write(&regs->gaddr2, 0xffffffff);
		gfar_write(&regs->gaddr3, 0xffffffff);
		gfar_write(&regs->gaddr4, 0xffffffff);
		gfar_write(&regs->gaddr5, 0xffffffff);
		gfar_write(&regs->gaddr6, 0xffffffff);
		gfar_write(&regs->gaddr7, 0xffffffff);
	} else {
		int em_num;
		int idx;

		/* zero out the hash */
		gfar_write(&regs->igaddr0, 0x0);
		gfar_write(&regs->igaddr1, 0x0);
		gfar_write(&regs->igaddr2, 0x0);
		gfar_write(&regs->igaddr3, 0x0);
		gfar_write(&regs->igaddr4, 0x0);
		gfar_write(&regs->igaddr5, 0x0);
		gfar_write(&regs->igaddr6, 0x0);
		gfar_write(&regs->igaddr7, 0x0);
		gfar_write(&regs->gaddr0, 0x0);
		gfar_write(&regs->gaddr1, 0x0);
		gfar_write(&regs->gaddr2, 0x0);
		gfar_write(&regs->gaddr3, 0x0);
		gfar_write(&regs->gaddr4, 0x0);
		gfar_write(&regs->gaddr5, 0x0);
		gfar_write(&regs->gaddr6, 0x0);
		gfar_write(&regs->gaddr7, 0x0);

		/* If we have extended hash tables, we need to
		 * clear the exact match registers to prepare for
		 * setting them */
		if (priv->extended_hash) {
			em_num = GFAR_EM_NUM + 1;
			gfar_clear_exact_match(dev);
			idx = 1;
		} else {
			idx = 0;
			em_num = 0;
		}

		if (netdev_mc_empty(dev))
			return;

		/* Parse the list, and set the appropriate bits */
		netdev_for_each_mc_addr(ha, dev) {
			if (idx < em_num) {
				gfar_set_mac_for_addr(dev, idx, ha->addr);
				idx++;
			} else
				gfar_set_hash_for_addr(dev, ha->addr);
		}
	}
}


/* Clears each of the exact match registers to zero, so they
 * don't interfere with normal reception */
static void gfar_clear_exact_match(struct net_device *dev)
{
	int idx;
	static const u8 zero_arr[MAC_ADDR_LEN] = {0, 0, 0, 0, 0, 0};

	for(idx = 1;idx < GFAR_EM_NUM + 1;idx++)
		gfar_set_mac_for_addr(dev, idx, zero_arr);
}

/* Set the appropriate hash bit for the given addr */
/* The algorithm works like so:
 * 1) Take the Destination Address (ie the multicast address), and
 * do a CRC on it (little endian), and reverse the bits of the
 * result.
 * 2) Use the 8 most significant bits as a hash into a 256-entry
 * table.  The table is controlled through 8 32-bit registers:
 * gaddr0-7.  gaddr0's MSB is entry 0, and gaddr7's LSB is
 * gaddr7.  This means that the 3 most significant bits in the
 * hash index which gaddr register to use, and the 5 other bits
 * indicate which bit (assuming an IBM numbering scheme, which
 * for PowerPC (tm) is usually the case) in the register holds
 * the entry. */
static void gfar_set_hash_for_addr(struct net_device *dev, u8 *addr)
{
	u32 tempval;
	struct gfar_private *priv = netdev_priv(dev);
	u32 result = ether_crc(MAC_ADDR_LEN, addr);
	int width = priv->hash_width;
	u8 whichbit = (result >> (32 - width)) & 0x1f;
	u8 whichreg = result >> (32 - width + 5);
	u32 value = (1 << (31-whichbit));

	tempval = gfar_read(priv->hash_regs[whichreg]);
	tempval |= value;
	gfar_write(priv->hash_regs[whichreg], tempval);
}


/* There are multiple MAC Address register pairs on some controllers
 * This function sets the numth pair to a given address
 */
static void gfar_set_mac_for_addr(struct net_device *dev, int num,
				  const u8 *addr)
{
	struct gfar_private *priv = netdev_priv(dev);
	struct gfar __iomem *regs = priv->gfargrp[0].regs;
	int idx;
	char tmpbuf[MAC_ADDR_LEN];
	u32 tempval;
	u32 __iomem *macptr = &regs->macstnaddr1;

	macptr += num*2;

	/* Now copy it into the mac registers backwards, cuz */
	/* little endian is silly */
	for (idx = 0; idx < MAC_ADDR_LEN; idx++)
		tmpbuf[MAC_ADDR_LEN - 1 - idx] = addr[idx];

	gfar_write(macptr, *((u32 *) (tmpbuf)));

	tempval = *((u32 *) (tmpbuf + 4));

	gfar_write(macptr+1, tempval);
}

/* GFAR error interrupt handler */
static irqreturn_t gfar_error(int irq, void *grp_id)
{
	struct gfar_priv_grp *gfargrp = grp_id;
	struct gfar __iomem *regs = gfargrp->regs;
	struct gfar_private *priv= gfargrp->priv;
	struct net_device *dev = priv->ndev;

	/* Save ievent for future reference */
	u32 events = gfar_read(&regs->ievent);

	/* Clear IEVENT */
	gfar_write(&regs->ievent, events & IEVENT_ERR_MASK);

	/* Magic Packet is not an error. */
	if ((priv->device_flags & FSL_GIANFAR_DEV_HAS_MAGIC_PACKET) &&
	    (events & IEVENT_MAG))
		events &= ~IEVENT_MAG;

	/* Hmm... */
	if (netif_msg_rx_err(priv) || netif_msg_tx_err(priv))
		netdev_dbg(dev, "error interrupt (ievent=0x%08x imask=0x%08x)\n",
			   events, gfar_read(&regs->imask));

	/* Update the error counters */
	if (events & IEVENT_TXE) {
		dev->stats.tx_errors++;

		if (events & IEVENT_LC)
			dev->stats.tx_window_errors++;
		if (events & IEVENT_CRL)
			dev->stats.tx_aborted_errors++;
		if (events & IEVENT_XFUN) {
			unsigned long flags;

			netif_dbg(priv, tx_err, dev,
				  "TX FIFO underrun, packet dropped\n");
			dev->stats.tx_dropped++;
			priv->extra_stats.tx_underrun++;

			local_irq_save(flags);
			lock_tx_qs(priv);

			/* Reactivate the Tx Queues */
			gfar_write(&regs->tstat, gfargrp->tstat);

			unlock_tx_qs(priv);
			local_irq_restore(flags);
		}
		netif_dbg(priv, tx_err, dev, "Transmit Error\n");
	}
	if (events & IEVENT_BSY) {
		dev->stats.rx_errors++;
		priv->extra_stats.rx_bsy++;

		gfar_receive(irq, grp_id);

		netif_dbg(priv, rx_err, dev, "busy error (rstat: %x)\n",
			  gfar_read(&regs->rstat));
	}
	if (events & IEVENT_BABR) {
		dev->stats.rx_errors++;
		priv->extra_stats.rx_babr++;

		netif_dbg(priv, rx_err, dev, "babbling RX error\n");
	}
	if (events & IEVENT_EBERR) {
		priv->extra_stats.eberr++;
		netif_dbg(priv, rx_err, dev, "bus error\n");
	}
	if (events & IEVENT_RXC)
		netif_dbg(priv, rx_status, dev, "control frame\n");

	if (events & IEVENT_BABT) {
		priv->extra_stats.tx_babt++;
		netif_dbg(priv, tx_err, dev, "babbling TX error\n");
	}
	return IRQ_HANDLED;
}

static struct of_device_id gfar_match[] =
{
	{
		.type = "network",
		.compatible = "gianfar",
	},
	{
		.compatible = "fsl,etsec2",
	},
	{},
};
MODULE_DEVICE_TABLE(of, gfar_match);

/* Structure for a device driver */
static struct platform_driver gfar_driver = {
	.driver = {
		.name = "fsl-gianfar",
		.owner = THIS_MODULE,
		.pm = GFAR_PM_OPS,
		.of_match_table = gfar_match,
	},
	.probe = gfar_probe,
	.remove = gfar_remove,
};

static int __init gfar_init(void)
{
	gfar_global_recycle_cntxt = gfar_init_recycle_cntxt();
	if (!gfar_global_recycle_cntxt)
		return -ENOMEM;
#ifdef CONFIG_RX_TX_BUFF_XCHG
	tx_napi_enabled = 0;
#endif
	return platform_driver_register(&gfar_driver);
}

static void __exit gfar_exit(void)
{
	gfar_free_recycle_cntxt(gfar_global_recycle_cntxt);
	platform_driver_unregister(&gfar_driver);
}

module_init(gfar_init);
module_exit(gfar_exit);

