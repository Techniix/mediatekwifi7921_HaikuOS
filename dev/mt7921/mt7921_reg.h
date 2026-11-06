/*
 * Register definitions for MediaTek MT7921/MT7922, taken from mt76
 * (mt792x_regs.h, mt7921/regs.h, mt7921/pci.c).
 *
 * Copyright (C) 2020-2023 MediaTek Inc.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */
#ifndef _MT7921_REG_H_
#define _MT7921_REG_H_

#ifndef BIT
#define BIT(n)			(1U << (n))
#endif

/* --- Chip identification (accessed through the L1 remap window) --- */
#define MT_HW_BOUND		0x70010020
#define MT_HW_CHIPID		0x70010200
#define MT_HW_REV		0x70010204

/* --- Host/firmware ownership handshake (CONN_INFRA conn_host_csr_top) --- */
#define MT_CONN_ON_LPCTL	0x7c060010
#define PCIE_LPCR_HOST_SET_OWN	BIT(0)
#define PCIE_LPCR_HOST_CLR_OWN	BIT(1)
#define PCIE_LPCR_HOST_OWN_SYNC	BIT(2)
#define MT792X_DRV_OWN_RETRY_COUNT 10

#define MT_CONN_ON_MISC		0x7c0600f0
#define MT_TOP_MISC2_FW_N9_RDY	0x3	/* GENMASK(1, 0) */

/* --- WF subsystem reset (connac2: 0x18000140) --- */
#define MT_WFSYS_SW_RST		0x18000140
#define WFSYS_SW_RST_B		BIT(0)
#define WFSYS_SW_INIT_DONE	BIT(4)

/* --- L1 remap window --- */
#define MT_INFRA_CFG_BASE	0xfe000
#define MT_INFRA(ofs)		(MT_INFRA_CFG_BASE + (ofs))
#define MT_HIF_REMAP_L1		MT_INFRA(0x24c)
#define MT_HIF_REMAP_L1_MASK	0x0000ffffU	/* GENMASK(15, 0)  */
#define MT_HIF_REMAP_L1_OFFSET	0x0000ffffU	/* GENMASK(15, 0)  */
#define MT_HIF_REMAP_L1_BASE	0xffff0000U	/* GENMASK(31, 16) */
#define MT_HIF_REMAP_BASE_L1	0x40000

/* --- WFDMA0 (mt7921/regs.h, mt792x_regs.h) --- */
#define MT_WFDMA0_BASE		0xd4000
#define MT_WFDMA0(ofs)		(MT_WFDMA0_BASE + (ofs))

#define MT_WFDMA0_RST		MT_WFDMA0(0x100)
#define MT_WFDMA0_RST_LOGIC_RST		BIT(4)
#define MT_WFDMA0_RST_DMASHDL_ALL_RST	BIT(5)

#define MT_MCU_CMD		MT_WFDMA0(0x1f0)
#define MT_MCU_CMD_WAKE_RX_PCIE	BIT(0)
#define MT_MCU2HOST_SW_INT_ENA	MT_WFDMA0(0x1f4)

#define MT_WFDMA0_HOST_INT_STA	MT_WFDMA0(0x200)	/* write-1-to-clear */
#define MT_WFDMA0_HOST_INT_ENA	MT_WFDMA0(0x204)

#define MT_WFDMA0_GLO_CFG	MT_WFDMA0(0x208)
#define MT_WFDMA0_GLO_CFG_TX_DMA_EN		BIT(0)
#define MT_WFDMA0_GLO_CFG_TX_DMA_BUSY		BIT(1)
#define MT_WFDMA0_GLO_CFG_RX_DMA_EN		BIT(2)
#define MT_WFDMA0_GLO_CFG_RX_DMA_BUSY		BIT(3)
#define MT_WFDMA0_GLO_CFG_DMA_SIZE_SHIFT	4	/* GENMASK(5, 4) */
#define MT_WFDMA0_GLO_CFG_TX_WB_DDONE		BIT(6)
#define MT_WFDMA0_GLO_CFG_FIFO_DIS_CHECK	BIT(11)
#define MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN	BIT(12)
#define MT_WFDMA0_GLO_CFG_RX_WB_DDONE		BIT(13)
#define MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN BIT(15)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2	BIT(21)
#define MT_WFDMA0_GLO_CFG_OMIT_RX_INFO		BIT(27)
#define MT_WFDMA0_GLO_CFG_OMIT_TX_INFO		BIT(28)
#define MT_WFDMA0_GLO_CFG_CLK_GAT_DIS		BIT(30)

#define MT_WFDMA0_RST_DTX_PTR	MT_WFDMA0(0x20c)
#define MT_WFDMA0_GLO_CFG_EXT0	MT_WFDMA0(0x2b0)
#define MT_WFDMA0_GLO_CFG_EXT0_CSR_TX_DMASHDL_EN BIT(6)
#define MT_WFDMA0_PRI_DLY_INT_CFG0 MT_WFDMA0(0x2f0)

#define MT_WFDMA0_TX_RING_EXT_CTRL(n)	MT_WFDMA0(0x600 + ((n) << 2))
#define MT_WFDMA0_RX_RING_EXT_CTRL(n)	MT_WFDMA0(0x680 + ((n) << 2))
#define MT_PREFETCH(base, depth)	(((uint32_t)(base) << 16) | (depth))

/* Ring register blocks: 16 bytes per ring (desc_base, ring_size, cpu_idx, dma_idx). */
#define MT_TX_RING_BASE		MT_WFDMA0(0x300)
#define MT_RX_EVENT_RING_BASE	MT_WFDMA0(0x500)
#define MT_RX_DATA_RING_BASE	MT_WFDMA0(0x520)
#define MT_RX_WA_RING_BASE	MT_WFDMA0(0x540)
#define MT_RING_SIZE		0x10
#define MT_RING_DESC_BASE	0x0
#define MT_RING_NDESC		0x4
#define MT_RING_CPU_IDX		0x8
#define MT_RING_DMA_IDX		0xc

/* Hardware queue indexes (mt7921/mt7921.h). */
#define MT7921_TXQ_BAND0	0
#define MT7921_TXQ_FWDL		16
#define MT7921_TXQ_MCU_WM	17

/* DMA scheduler (CONN_INFRA). */
#define MT_DMASHDL_SW_CONTROL	0x7c026004
#define MT_DMASHDL_DMASHDL_BYPASS BIT(28)

/* Written by the driver when WFDMA has been set up. */
#define MT_WFDMA_DUMMY_CR	(0x54000000 + 0x120)
#define MT_WFDMA_NEED_REINIT	BIT(1)

/* --- DMA descriptor (mt76/dma.h) --- */
#define MT_DMA_CTL_SD_LEN1	0x00003fffU	/* GENMASK(13, 0)  */
#define MT_DMA_CTL_LAST_SEC1	BIT(14)
#define MT_DMA_CTL_SD_LEN0_SHIFT 16
#define MT_DMA_CTL_SD_LEN0	0x3fff0000U	/* GENMASK(29, 16) */
#define MT_DMA_CTL_LAST_SEC0	BIT(30)
#define MT_DMA_CTL_DMA_DONE	BIT(31)

/* --- Firmware / MCU state (mt792x_regs.h) --- */
#define MT_SWDEF_MODE		(0x41f200 + 0x3c)
#define MT_SWDEF_NORMAL_MODE	0
#define MT_TOP_LPCR_HOST_BAND0	(0x18060000 + 0x10)
#define MT_TOP_LPCR_HOST_FW_OWN	BIT(0)
#define MT_TOP_LPCR_HOST_DRV_OWN BIT(1)
#define MT_TOP_MISC_FW_STATE	0x7	/* GENMASK(2, 0), in MT_CONN_ON_MISC */
#define MT_TOP_MISC2_FW_PWR_ON	BIT(0)
#define MT_PCIE_MAC_PM_L0S_DIS	BIT(8)

/* --- PCIe MAC --- */
#define MT_PCIE_MAC_BASE	0x10000
#define MT_PCIE_MAC(ofs)	(MT_PCIE_MAC_BASE + (ofs))
#define MT_PCIE_MAC_INT_ENABLE	MT_PCIE_MAC(0x188)
#define MT_PCIE_MAC_PM		MT_PCIE_MAC(0x194)

/*
 * Fixed chip-address -> BAR0-offset map, copied verbatim from
 * __mt7921_reg_addr() in mt76/mt7921/pci.c.
 */
struct mt7921_reg_map {
	uint32_t phys;	/* chip address */
	uint32_t maps;	/* BAR0 offset */
	uint32_t size;
};

static const struct mt7921_reg_map mt7921_fixed_map[] = {
	{ 0x820d0000, 0x30000, 0x10000 }, /* WF_LMAC_TOP (WF_WTBLON) */
	{ 0x820ed000, 0x24800, 0x00800 }, /* WF_LMAC_TOP BN0 (WF_MIB) */
	{ 0x820e4000, 0x21000, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_TMAC) */
	{ 0x820e7000, 0x21e00, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_DMA) */
	{ 0x820eb000, 0x24200, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_LPON) */
	{ 0x820e2000, 0x20800, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_AGG) */
	{ 0x820e3000, 0x20c00, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_ARB) */
	{ 0x820e5000, 0x21400, 0x00800 }, /* WF_LMAC_TOP BN0 (WF_RMAC) */
	{ 0x00400000, 0x80000, 0x10000 }, /* WF_MCU_SYSRAM */
	{ 0x00410000, 0x90000, 0x10000 }, /* WF_MCU_SYSRAM (configure register) */
	{ 0x40000000, 0x70000, 0x10000 }, /* WF_UMAC_SYSRAM */
	{ 0x54000000, 0x02000, 0x01000 }, /* WFDMA PCIE0 MCU DMA0 */
	{ 0x55000000, 0x03000, 0x01000 }, /* WFDMA PCIE0 MCU DMA1 */
	{ 0x58000000, 0x06000, 0x01000 }, /* WFDMA PCIE1 MCU DMA0 (MEM_DMA) */
	{ 0x59000000, 0x07000, 0x01000 }, /* WFDMA PCIE1 MCU DMA1 */
	{ 0x7c000000, 0xf0000, 0x10000 }, /* CONN_INFRA */
	{ 0x7c020000, 0xd0000, 0x10000 }, /* CONN_INFRA, WFDMA */
	{ 0x7c060000, 0xe0000, 0x10000 }, /* CONN_INFRA, conn_host_csr_top */
	{ 0x80020000, 0xb0000, 0x10000 }, /* WF_TOP_MISC_OFF */
	{ 0x81020000, 0xc0000, 0x10000 }, /* WF_TOP_MISC_ON */
	{ 0x820c0000, 0x08000, 0x04000 }, /* WF_UMAC_TOP (PLE) */
	{ 0x820c8000, 0x0c000, 0x02000 }, /* WF_UMAC_TOP (PSE) */
	{ 0x820cc000, 0x0e000, 0x01000 }, /* WF_UMAC_TOP (PP) */
	{ 0x820cd000, 0x0f000, 0x01000 }, /* WF_MDP_TOP */
	{ 0x74030000, 0x10000, 0x10000 }, /* PCIE_MAC_IREG */
	{ 0x820ce000, 0x21c00, 0x00200 }, /* WF_LMAC_TOP (WF_SEC) */
	{ 0x820cf000, 0x22000, 0x01000 }, /* WF_LMAC_TOP (WF_PF) */
	{ 0x820e0000, 0x20000, 0x00400 }, /* WF_LMAC_TOP BN0 (WF_CFG) */
	{ 0x820e1000, 0x20400, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_TRB) */
	{ 0x820e9000, 0x23400, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_WTBLOFF) */
	{ 0x820ea000, 0x24000, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_ETBF) */
	{ 0x820ec000, 0x24600, 0x00200 }, /* WF_LMAC_TOP BN0 (WF_INT) */
	{ 0x820f0000, 0xa0000, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_CFG) */
	{ 0x820f1000, 0xa0600, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_TRB) */
	{ 0x820f2000, 0xa0800, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_AGG) */
	{ 0x820f3000, 0xa0c00, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_ARB) */
	{ 0x820f4000, 0xa1000, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_TMAC) */
	{ 0x820f5000, 0xa1400, 0x00800 }, /* WF_LMAC_TOP BN1 (WF_RMAC) */
	{ 0x820f7000, 0xa1e00, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_DMA) */
	{ 0x820f9000, 0xa3400, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_WTBLOFF) */
	{ 0x820fa000, 0xa4000, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_ETBF) */
	{ 0x820fb000, 0xa4200, 0x00400 }, /* WF_LMAC_TOP BN1 (WF_LPON) */
	{ 0x820fc000, 0xa4600, 0x00200 }, /* WF_LMAC_TOP BN1 (WF_INT) */
	{ 0x820fd000, 0xa4800, 0x00800 }, /* WF_LMAC_TOP BN1 (WF_MIB) */
};

/* --- MAC (mt7921/regs.h, mt792x_regs.h) --- */
#define MT_MDP_BASE			0x820cd000
#define MT_MDP_DCR1			(MT_MDP_BASE + 0x004)
#define MT_MDP_DCR1_MAX_RX_LEN		0x0000fff8U	/* GENMASK(15, 3) */

#define MT_WF_TMAC_BASE(b)		((b) ? 0x820f4000 : 0x820e4000)
#define MT_TMAC_CTCR0(b)		(MT_WF_TMAC_BASE(b) + 0x0f4)
#define MT_TMAC_CTCR0_INS_DDLMT_REFTIME	0x0000003fU	/* GENMASK(5, 0) */
#define MT_TMAC_CTCR0_INS_DDLMT_EN	BIT(17)
#define MT_TMAC_CTCR0_INS_DDLMT_VHT_SMPDU_EN BIT(18)

#define MT_WF_DMA_BASE(b)		((b) ? 0x820f7000 : 0x820e7000)
#define MT_DMA_DCR0(b)			(MT_WF_DMA_BASE(b) + 0x000)
#define MT_DMA_DCR0_MAX_RX_LEN		0x0000fff8U	/* GENMASK(15, 3) */
#define MT_DMA_DCR0_RXD_G5_EN		BIT(23)

#define MT_WTBLOFF_TOP_BASE(b)		((b) ? 0x820f9000 : 0x820e9000)
#define MT_WTBLOFF_TOP_RSCR(b)		(MT_WTBLOFF_TOP_BASE(b) + 0x008)
#define MT_WTBLOFF_TOP_RSCR_RCPI_MODE	0xc0000000U	/* GENMASK(31, 30) */
#define MT_WTBLOFF_TOP_RSCR_RCPI_PARAM	0x03000000U	/* GENMASK(25, 24) */

#define MT_WF_MIB_BASE(b)		((b) ? 0x820fd000 : 0x820ed000)
#define MT_MIB_SCR1(b)			(MT_WF_MIB_BASE(b) + 0x004)
#define MT_MIB_TXDUR_EN			BIT(8)
#define MT_MIB_RXDUR_EN			BIT(9)

#define MT_WF_RMAC_BASE(b)		((b) ? 0x820f5000 : 0x820e5000)
#define MT_WF_RMAC_MIB_TIME0(b)		(MT_WF_RMAC_BASE(b) + 0x03c4)
#define MT_WF_RMAC_MIB_AIRTIME0(b)	(MT_WF_RMAC_BASE(b) + 0x0380)
#define MT_WF_RMAC_MIB_RXTIME_EN	BIT(30)

#define MT_WTBL_UPDATE			(0x820d4000 + 0x230)
#define MT_WTBL_UPDATE_WLAN_IDX		0x000003ffU
#define MT_WTBL_UPDATE_ADM_COUNT_CLEAR	BIT(12)
#define MT_WTBL_UPDATE_BUSY		BIT(31)

/* --- RX descriptor (mt76_connac2_mac.h) --- */
#define MT_RXD1_NORMAL_WLAN_IDX		0x000003ffU
#define MT_RXD1_NORMAL_GROUP_1		BIT(11)
#define MT_RXD1_NORMAL_GROUP_2		BIT(12)
#define MT_RXD1_NORMAL_GROUP_3		BIT(13)
#define MT_RXD1_NORMAL_GROUP_4		BIT(14)
#define MT_RXD1_NORMAL_GROUP_5		BIT(15)
#define MT_RXD1_NORMAL_SEC_MODE_SHIFT	16		/* GENMASK(20, 16) */
#define MT_RXD1_NORMAL_CM		BIT(23)
#define MT_RXD1_NORMAL_CLM		BIT(24)
#define MT_RXD1_NORMAL_ICV_ERR		BIT(25)
#define MT_RXD1_NORMAL_TKIP_MIC_ERR	BIT(26)
#define MT_RXD1_NORMAL_FCS_ERR		BIT(27)
#define MT_RXD1_NORMAL_BAND_IDX		BIT(28)
#define MT_RXD2_NORMAL_HDR_TRANS	BIT(13)
#define MT_RXD2_NORMAL_HDR_OFFSET_SHIFT	14		/* GENMASK(15, 14) */
#define MT_RXD2_NORMAL_AMSDU_ERR	BIT(23)
#define MT_RXD2_NORMAL_MAX_LEN_ERROR	BIT(24)
#define MT_RXD3_NORMAL_CH_FREQ_SHIFT	8		/* GENMASK(15, 8) */
#define MT_RXD4_NORMAL_PAYLOAD_FORMAT	0x3U

#endif /* _MT7921_REG_H_ */
