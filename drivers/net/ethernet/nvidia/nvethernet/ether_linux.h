/*
 * Copyright (c) 2018-2019, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ETHER_LINUX_H
#define ETHER_LINUX_H

#include <linux/platform_device.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/of_gpio.h>
#include <linux/of_mdio.h>
#include <linux/if_vlan.h>
#include <linux/thermal.h>
#include <linux/of_net.h>
#include <linux/module.h>
#include <linux/reset.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/of.h>

#include <osi_core.h>
#include <osi_dma.h>
#include "ioctl.h"

#define ETHER_MAX_IRQS			4
#define ETHER_IRQ_MAX_IDX		8
#define ETHER_IRQ_NAME_SZ		32
#define ETHER_QUEUE_PRIO_DEFAULT	0U
#define ETHER_QUEUE_PRIO_MAX		7U

/* Map max. 4KB buffer per Tx descriptor */
#define ETHER_MAX_DATA_LEN_PER_TXD_BUF BIT(12)

/* Incase of TSO/GSO, Tx ring needs atmost MAX_SKB_FRAGS +
 * one context descriptor +
 * one descriptor for header/linear buffer payload
 */
#define TX_DESC_THRESHOLD	(MAX_SKB_FRAGS + 2)

/**
 *	ether_avail_txdesc_count - Return count of available tx desc.
 *	@tx_ring: Tx ring instance associated with channel number
 *
 *	Algorithm: Check the difference between current desc index
 *	and the desc. index to be cleaned.
 *
 *	Dependencies: MAC needs to be initialized and Tx ring allocated.
 *
 *	Protection: None.
 *
 *	Return: Number of available descriptors in the given Tx ring.
 */
static inline int ether_avail_txdesc_cnt(struct osi_tx_ring *tx_ring)
{
	return ((tx_ring->clean_idx - tx_ring->cur_tx_idx - 1) &
		(TX_DESC_CNT - 1));
}

#ifdef THERMAL_CAL
/* The DT binding for ethernet device has 5 thermal zones in steps of
 * 35 degress from -40C to 110C. Each zone corresponds to a state.
 */
#define ETHER_MAX_THERM_STATE		5UL
#endif /* THERMAL_CAL */

/**
 *	struct ether_tx_napi - DMA Transmit Channel NAPI
 *	@chan: Transmit channel number
 *	@pdata: OSD private data
 *	@napi: NAPI instance associated with transmit channel
 */
struct ether_tx_napi {
	unsigned int chan;
	struct ether_priv_data *pdata;
	struct napi_struct napi;
};

/**
 *	struct ether_rx_napi - DMA Receive Channel NAPI
 *	@chan: Receive channel number
 *	@pdata: OSD Private data
 *	@napi: NAPI instance associated with receive channel
 */
struct ether_rx_napi {
	unsigned int chan;
	struct ether_priv_data *pdata;
	struct napi_struct napi;
};

/**
 *	struct ether_priv_data - Ethernet driver private data
 *	@osi_core:	OSI core private data
 *	@osi_dma:	OSI DMA private data
 *	@hwfeat:	HW supported feature list
 *	@tx_napi:	Array of DMA Transmit channel NAPI
 *	@rx_napi:	Array of DMA Receive channel NAPI
 *	@ndev:	Network device associated with driver
 *	@dev:	Base device associated with driver
 *	@mac_rst:	Reset for the MAC
 *	@pllrefe_clk:	PLLREFE clock
 *	@axi_clk:	Clock from AXI
 *	@axi_cbb_clk:	Clock from AXI CBB
 *	@rx_clk:	Receive clock (which will be driven from the PHY)
 *	@ptp_ref_clk:	PTP reference clock from AXI
 *	@tx_clk:	Transmit clock
 *	@phy_node:	Pointer to PHY device tree node
 *	@mdio_node:	Pointer to MDIO device tree node
 *	@mii:		Pointer to MII bus instance
 *	@phydev:	Pointer to the PHY device
 *	@interface:	Interface type assciated with MAC (SGMII/RGMII/...)
 *			this information will be provided with phy-mode DT
 *			entry
 *	@oldlink:	Previous detected link
 *	@speed:		PHY link speed
 *	@oldduplex:	Previous detected mode
 *	@phy_reset:	Reset for PHY
 *	@rx_irq_alloc_mask:	Tx IRQ alloc mask
 *	@tx_irq_alloc_mask:	Rx IRQ alloc mask
 *	@common_irq:	Common IRQ number for MAC
 *	@tx_irqs:	Array of DMA Transmit channel IRQ numbers
 *	@rx_irqs:	Array of DMA Receive channel IRQ numbers
 *	@dma_mask:	memory allocation mask
 *	@mac_loopback_mode:	MAC loopback mode
 *	@q_prio:	Array of MTL queue TX priority
 *	@hw_feat_cur_state:	Current state of features enabled in HW
 *	@tcd:		Pointer to thermal cooling device which this driver
 *			registers with the kernel. Kernel will invoke the
 *			callback ops for this cooling device when temperate
 *			in thermal zone defined in DT binding for this driver
 *			is tripped.
 *	@therm_state:	Atomic variable to hold the current temperature zone
 *			which has triggered.
 */
struct ether_priv_data {
	struct osi_core_priv_data *osi_core;
	struct osi_dma_priv_data *osi_dma;

	struct osi_hw_features hw_feat;
	struct ether_tx_napi *tx_napi[OSI_EQOS_MAX_NUM_CHANS];
	struct ether_rx_napi *rx_napi[OSI_EQOS_MAX_NUM_CHANS];

	struct net_device *ndev;
	struct device *dev;

	struct reset_control *mac_rst;
	struct clk *pllrefe_clk;
	struct clk *axi_clk;
	struct clk *axi_cbb_clk;
	struct clk *rx_clk;
	struct clk *ptp_ref_clk;
	struct clk *tx_clk;

	struct device_node *phy_node;
	struct device_node *mdio_node;
	struct mii_bus *mii;
	struct phy_device *phydev;
	int interface;
	unsigned int oldlink;
	int speed;
	int oldduplex;
	int phy_reset;

	unsigned int rx_irq_alloc_mask;
	unsigned int tx_irq_alloc_mask;
	unsigned int common_irq_alloc_mask;

	int common_irq;
	int tx_irqs[ETHER_MAX_IRQS];
	int rx_irqs[ETHER_MAX_IRQS];
	unsigned long long dma_mask;
	netdev_features_t hw_feat_cur_state;

	/* for MAC loopback */
	unsigned int mac_loopback_mode;
	unsigned int q_prio[OSI_EQOS_MAX_NUM_CHANS];

#ifdef THERMAL_CAL
	struct thermal_cooling_device *tcd;
	atomic_t therm_state;
#endif /* THERMAL_CAL */
};

void ether_set_ethtool_ops(struct net_device *ndev);
int ether_sysfs_register(struct device *dev);
void ether_sysfs_unregister(struct device *dev);
#endif /* ETHER_LINUX_H */
