/*
 * Copyright 2026, Techniix. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Portions derived from MediaTek's mt76 driver (dma.c, mt792x_dma.c,
 * mt7921/pci.c):
 *   Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 *   Copyright (C) 2020-2023 MediaTek Inc.
 *   SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * WFDMA ring handling for MT7921/MT7922.
 *
 * This is a deliberately simple, polled implementation: every ring uses one
 * physically contiguous descriptor array and one contiguous buffer area
 * (one fixed-size buffer per descriptor), all below 4 GB, as the chip only
 * does 32-bit DMA here (upstream sets a 32-bit DMA mask). TX is synchronous:
 * mt7921_dma_tx() waits until the chip has fetched the descriptor. Nothing
 * here uses interrupts; they stay masked until the data path exists.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/rman.h>

#include <machine/atomic.h>
#include <machine/bus.h>

#include <dev/mt7921/if_mt7921var.h>
#include <dev/mt7921/mt7921_reg.h>


/* Ring sizes. Upstream uses much larger rings; these are enough for now. */
#define MT7921_TX_DATA0_NDESC	MT7921_TX_SLOTS
#define MT7921_TX_DATA_BUF_SIZE	2048	/* TXD + TXP (64) + one 802.11 frame */
#define MT7921_TX_MCU_NDESC	16
#define MT7921_TX_FWDL_NDESC	16
#define MT7921_RX_MCU_NDESC	8	/* same as upstream */
#define MT7921_RX_WA_NDESC	64
#define MT7921_RX_DATA_NDESC	32

#define MT7921_TX_BUF_SIZE	4096	/* largest FW_SCATTER chunk */
#define MT7921_RX_BUF_SIZE	2048	/* MT_RX_BUF_SIZE */

#define MT7921_DMA_4G		0x100000000ULL


/* #pragma mark - contiguous DMA memory */


static void
mt7921_dma_map_cb(void* arg, bus_dma_segment_t* segs, int nseg, int error)
{
	if (error != 0 || nseg != 1)
		return;
	*(bus_addr_t*)arg = segs[0].ds_addr;
}


static int
mt7921_dma_alloc(struct mt7921_softc* sc, bus_size_t size, bus_size_t align,
	bus_dma_tag_t* tag, bus_dmamap_t* map, void** vaddr, bus_addr_t* paddr)
{
	int error;

	*paddr = 0;
	error = bus_dma_tag_create(bus_get_dma_tag(sc->sc_dev), align, 0,
		BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
		size, 1, size, 0, NULL, NULL, tag);
	if (error != 0)
		return error;

	error = bus_dmamem_alloc(*tag, vaddr,
		BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, map);
	if (error != 0) {
		bus_dma_tag_destroy(*tag);
		*tag = NULL;
		return error;
	}

	error = bus_dmamap_load(*tag, *map, *vaddr, size, mt7921_dma_map_cb,
		paddr, BUS_DMA_NOWAIT);
	if (error != 0 || *paddr == 0
		|| (uint64_t)*paddr + size > MT7921_DMA_4G) {
		device_printf(sc->sc_dev, "DMA memory not usable (paddr 0x%jx)\n",
			(uintmax_t)*paddr);
		if (error == 0)
			bus_dmamap_unload(*tag, *map);
		bus_dmamem_free(*tag, *vaddr, *map);
		bus_dma_tag_destroy(*tag);
		*tag = NULL;
		*vaddr = NULL;
		return error != 0 ? error : ENOMEM;
	}

	return 0;
}


static void
mt7921_dma_free(bus_dma_tag_t* tag, bus_dmamap_t map, void* vaddr)
{
	if (*tag == NULL)
		return;
	bus_dmamap_unload(*tag, map);
	bus_dmamem_free(*tag, vaddr, map);
	bus_dma_tag_destroy(*tag);
	*tag = NULL;
}


/* #pragma mark - rings */


static inline void
mt7921_ring_wr(struct mt7921_softc* sc, struct mt7921_ring* r, uint32_t reg,
	uint32_t val)
{
	mt7921_wr(sc, r->regs + reg, val);
}


static inline uint32_t
mt7921_ring_rr(struct mt7921_softc* sc, struct mt7921_ring* r, uint32_t reg)
{
	return mt7921_rr(sc, r->regs + reg);
}


/* Port of mt76_dma_queue_reset(reset_idx = true) + mt76_dma_sync_idx(). */
static void
mt7921_ring_reset(struct mt7921_softc* sc, struct mt7921_ring* r)
{
	uint32_t idx;
	int i;

	for (i = 0; i < r->ndesc; i++) {
		r->desc[i].buf0 = 0;
		r->desc[i].buf1 = 0;
		r->desc[i].info = 0;
		r->desc[i].ctrl = htole32(MT_DMA_CTL_DMA_DONE);
	}
	wmb();

	mt7921_ring_wr(sc, r, MT_RING_CPU_IDX, 0);
	mt7921_ring_wr(sc, r, MT_RING_DMA_IDX, 0);
	mt7921_ring_wr(sc, r, MT_RING_NDESC, r->ndesc);
	mt7921_ring_wr(sc, r, MT_RING_DESC_BASE, (uint32_t)r->desc_paddr);

	idx = mt7921_ring_rr(sc, r, MT_RING_DMA_IDX);
	r->head = r->tail = idx < (uint32_t)r->ndesc ? (int)idx : 0;
}


static int
mt7921_ring_alloc(struct mt7921_softc* sc, struct mt7921_ring* r,
	const char* name, uint32_t base, int hw_idx, int ndesc, int bufsize,
	bool is_rx)
{
	int error;

	memset(r, 0, sizeof(*r));
	r->name = name;
	r->regs = base + hw_idx * MT_RING_SIZE;
	r->hw_idx = hw_idx;
	r->ndesc = ndesc;
	r->bufsize = bufsize;
	r->is_rx = is_rx;

	error = mt7921_dma_alloc(sc, ndesc * sizeof(struct mt7921_desc), 16,
		&r->desc_tag, &r->desc_map, (void**)&r->desc, &r->desc_paddr);
	if (error != 0) {
		device_printf(sc->sc_dev, "%s: descriptor alloc failed (%d)\n",
			name, error);
		return error;
	}

	if (bufsize > 0) {
		error = mt7921_dma_alloc(sc, (bus_size_t)ndesc * bufsize, 64,
			&r->buf_tag, &r->buf_map, (void**)&r->buf, &r->buf_paddr);
		if (error != 0) {
			device_printf(sc->sc_dev, "%s: buffer alloc failed (%d)\n",
				name, error);
			return error;
		}
	}

	mt7921_ring_reset(sc, r);
	return 0;
}


static void
mt7921_ring_free(struct mt7921_ring* r)
{
	mt7921_dma_free(&r->buf_tag, r->buf_map, r->buf);
	mt7921_dma_free(&r->desc_tag, r->desc_map, (void*)r->desc);
	r->buf = NULL;
	r->desc = NULL;
}


static inline void
mt7921_rx_arm(struct mt7921_ring* r, int idx)
{
	volatile struct mt7921_desc* d = &r->desc[idx];

	d->buf0 = htole32((uint32_t)(r->buf_paddr + (bus_addr_t)idx * r->bufsize));
	d->buf1 = 0;
	d->info = 0;
	d->ctrl = htole32(((uint32_t)r->bufsize << MT_DMA_CTL_SD_LEN0_SHIFT)
		& MT_DMA_CTL_SD_LEN0);
}


/* Port of mt76_dma_rx_fill_buf(): hand all but one descriptor to the chip. */
static void
mt7921_rx_fill(struct mt7921_softc* sc, struct mt7921_ring* r)
{
	int i;

	for (i = 0; i < r->ndesc - 1; i++) {
		mt7921_rx_arm(r, r->head);
		r->head = (r->head + 1) % r->ndesc;
	}
	wmb();
	mt7921_ring_wr(sc, r, MT_RING_CPU_IDX, r->head);
}


/*
 * Queue one buffer on a TX ring and wait until the chip has consumed it
 * (DMA index caught up). Port of mt76_dma_tx_queue_skb_raw() +
 * mt76_dma_add_buf() + mt76_dma_kick_queue() for the single-buffer case.
 */
int
mt7921_dma_tx(struct mt7921_softc* sc, struct mt7921_ring* r,
	const void* data, int len)
{
	volatile struct mt7921_desc* d;
	int idx;

	if (!sc->sc_dma_up || r->buf == NULL)
		return ENXIO;
	if (sc->sc_dma_paused)
		return ENETDOWN;
	if (len <= 0 || len > r->bufsize)
		return EINVAL;

	idx = r->head;
	memcpy(r->buf + (size_t)idx * r->bufsize, data, len);

	d = &r->desc[idx];
	d->buf0 = htole32((uint32_t)(r->buf_paddr + (bus_addr_t)idx * r->bufsize));
	d->buf1 = 0;
	d->info = 0;
	d->ctrl = htole32((((uint32_t)len << MT_DMA_CTL_SD_LEN0_SHIFT)
		& MT_DMA_CTL_SD_LEN0) | MT_DMA_CTL_LAST_SEC0);

	r->head = (idx + 1) % r->ndesc;
	wmb();
	mt7921_ring_wr(sc, r, MT_RING_CPU_IDX, r->head);

	if (!mt7921_poll_msec(sc, r->regs + MT_RING_DMA_IDX, 0xfff, r->head,
			1000, 1)) {
		device_printf(sc->sc_dev, "%s: TX timeout (cpu %d, dma %u)\n",
			r->name, r->head,
			mt7921_ring_rr(sc, r, MT_RING_DMA_IDX));
		return ETIMEDOUT;
	}
	return 0;
}


/*
 * Asynchronous TX for the data ring (port of mt76_dma_tx_queue_skb() for the
 * connac "TXD + TXP" layout). Each slot holds the 64-byte TXWI followed by
 * the frame; the slot index doubles as the MSDU token, and the slot stays
 * busy until the firmware reports the token in a TX-free event, because
 * the chip fetches the frame through the TXP pointer some time after it
 * has consumed the ring descriptor. Called with sc_mtx held.
 */
int
mt7921_dma_tx_get_slot(struct mt7921_softc* sc, struct mt7921_ring* r,
	uint8_t** buf, bus_addr_t* paddr)
{
	int idx = r->head, next = (r->head + 1) % r->ndesc;
	uint32_t dma;

	if (!sc->sc_dma_up || sc->sc_dma_paused || r->buf == NULL)
		return -1;

	dma = mt7921_ring_rr(sc, r, MT_RING_DMA_IDX);
	if ((uint32_t)next == dma || r->busy[idx])
		return -1;			/* ring full / token still out */

	*buf = r->buf + (size_t)idx * r->bufsize;
	*paddr = r->buf_paddr + (bus_addr_t)idx * r->bufsize;
	return idx;
}


void
mt7921_dma_tx_post(struct mt7921_softc* sc, struct mt7921_ring* r, int idx,
	int len)
{
	volatile struct mt7921_desc* d = &r->desc[idx];

	d->buf0 = htole32((uint32_t)(r->buf_paddr + (bus_addr_t)idx * r->bufsize));
	d->buf1 = 0;
	d->info = 0;
	d->ctrl = htole32((((uint32_t)len << MT_DMA_CTL_SD_LEN0_SHIFT)
		& MT_DMA_CTL_SD_LEN0) | MT_DMA_CTL_LAST_SEC0);
	r->busy[idx] = 1;

	r->head = (idx + 1) % r->ndesc;
	wmb();
	mt7921_ring_wr(sc, r, MT_RING_CPU_IDX, r->head);
}


void
mt7921_dma_tx_token_free(struct mt7921_softc* sc, struct mt7921_ring* r,
	int token)
{
	if (token >= 0 && token < r->ndesc && token < MT7921_TX_SLOTS)
		r->busy[token] = 0;
}


/*
 * Consume every completed RX descriptor, calling handler for each frame,
 * then give the descriptors back. Returns the sum of the handler's return
 * values. Port of mt76_dma_rx_process() / mt76_dma_dequeue() for
 * single-buffer frames.
 */
int
mt7921_dma_rx_poll(struct mt7921_softc* sc, struct mt7921_ring* r,
	int (*handler)(struct mt7921_softc*, const uint8_t*, int, void*),
	void* arg)
{
	int hits = 0, done = 0;

	if (!sc->sc_dma_up || r->buf == NULL)
		return 0;

	while (done < r->ndesc) {
		uint32_t ctrl = le32toh(r->desc[r->tail].ctrl);
		int len;

		if ((ctrl & MT_DMA_CTL_DMA_DONE) == 0)
			break;
		mb();

		len = (ctrl & MT_DMA_CTL_SD_LEN0) >> MT_DMA_CTL_SD_LEN0_SHIFT;
		if ((ctrl & MT_DMA_CTL_LAST_SEC0) == 0) {
			device_printf(sc->sc_dev, "%s: dropping multi-buffer frame\n",
				r->name);
		} else if (len > 0 && len <= r->bufsize && handler != NULL) {
			hits += handler(sc,
				r->buf + (size_t)r->tail * r->bufsize, len, arg);
		}

		/* The free slot is always head == tail - 1; re-arm it. */
		mt7921_rx_arm(r, r->head);
		r->head = (r->head + 1) % r->ndesc;
		r->tail = (r->tail + 1) % r->ndesc;
		done++;
	}

	if (done > 0) {
		wmb();
		mt7921_ring_wr(sc, r, MT_RING_CPU_IDX, r->head);
	}
	return hits;
}


/* #pragma mark - WFDMA engine */


/* Port of mt792x_dma_disable(force = true). */
static int
mt7921_dma_disable(struct mt7921_softc* sc)
{
	mt7921_clear(sc, MT_WFDMA0_GLO_CFG,
		MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN
		| MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN
		| MT_WFDMA0_GLO_CFG_OMIT_TX_INFO | MT_WFDMA0_GLO_CFG_OMIT_RX_INFO
		| MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);

	if (!mt7921_poll_msec(sc, MT_WFDMA0_GLO_CFG,
			MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY,
			0, 100, 1)) {
		device_printf(sc->sc_dev, "WFDMA stays busy\n");
		return ETIMEDOUT;
	}

	/* Disable the DMA scheduler. */
	mt7921_clear(sc, MT_WFDMA0_GLO_CFG_EXT0,
		MT_WFDMA0_GLO_CFG_EXT0_CSR_TX_DMASHDL_EN);
	mt7921_set(sc, MT_DMASHDL_SW_CONTROL, MT_DMASHDL_DMASHDL_BYPASS);

	/* Reset. */
	mt7921_clear(sc, MT_WFDMA0_RST,
		MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
	mt7921_set(sc, MT_WFDMA0_RST,
		MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);

	return 0;
}


/* Port of mt792x_dma_prefetch() for MT7921/MT7922. */
static void
mt7921_dma_prefetch(struct mt7921_softc* sc)
{
	/* rx rings */
	mt7921_wr(sc, MT_WFDMA0_RX_RING_EXT_CTRL(0), MT_PREFETCH(0x0, 0x4));
	mt7921_wr(sc, MT_WFDMA0_RX_RING_EXT_CTRL(2), MT_PREFETCH(0x40, 0x4));
	mt7921_wr(sc, MT_WFDMA0_RX_RING_EXT_CTRL(3), MT_PREFETCH(0x80, 0x4));
	mt7921_wr(sc, MT_WFDMA0_RX_RING_EXT_CTRL(4), MT_PREFETCH(0xc0, 0x4));
	mt7921_wr(sc, MT_WFDMA0_RX_RING_EXT_CTRL(5), MT_PREFETCH(0x100, 0x4));
	/* tx rings */
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(0), MT_PREFETCH(0x140, 0x4));
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(1), MT_PREFETCH(0x180, 0x4));
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(2), MT_PREFETCH(0x1c0, 0x4));
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(3), MT_PREFETCH(0x200, 0x4));
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(4), MT_PREFETCH(0x240, 0x4));
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(5), MT_PREFETCH(0x280, 0x4));
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(6), MT_PREFETCH(0x2c0, 0x4));
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(16), MT_PREFETCH(0x340, 0x4));
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(17), MT_PREFETCH(0x380, 0x4));
}


/*
 * Port of mt792x_dma_enable() for MT7921/MT7922, except that host
 * interrupts stay masked (MT_WFDMA0_HOST_INT_ENA = 0): everything is polled.
 */
static void
mt7921_dma_enable(struct mt7921_softc* sc)
{
	mt7921_dma_prefetch(sc);

	/* reset dma idx */
	mt7921_wr(sc, MT_WFDMA0_RST_DTX_PTR, ~0U);

	/* configure delay interrupt */
	mt7921_wr(sc, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

	mt7921_set(sc, MT_WFDMA0_GLO_CFG,
		MT_WFDMA0_GLO_CFG_TX_WB_DDONE
		| MT_WFDMA0_GLO_CFG_FIFO_LITTLE_ENDIAN
		| MT_WFDMA0_GLO_CFG_CLK_GAT_DIS
		| MT_WFDMA0_GLO_CFG_OMIT_TX_INFO
		| (3U << MT_WFDMA0_GLO_CFG_DMA_SIZE_SHIFT)
		| MT_WFDMA0_GLO_CFG_FIFO_DIS_CHECK
		| MT_WFDMA0_GLO_CFG_RX_WB_DDONE
		| MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN
		| MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);

	mt7921_set(sc, MT_WFDMA0_GLO_CFG,
		MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);

	mt7921_set(sc, MT_WFDMA_DUMMY_CR, MT_WFDMA_NEED_REINIT);

	/* Keep host interrupts masked; only allow the MCU wake notification. */
	mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	mt7921_set(sc, MT_MCU2HOST_SW_INT_ENA, MT_MCU_CMD_WAKE_RX_PCIE);
}


/* Port of mt7921_dma_init() (mt7921/pci.c), with smaller rings. */
int
mt7921_dma_init(struct mt7921_softc* sc)
{
	int error;

	if ((error = mt7921_dma_disable(sc)) != 0)
		return error;

	/* TX data, band 0 (not used yet, but set up like upstream). */
	error = mt7921_ring_alloc(sc, &sc->sc_tx_data0, "tx-data0",
		MT_TX_RING_BASE, MT7921_TXQ_BAND0, MT7921_TX_DATA0_NDESC,
		MT7921_TX_DATA_BUF_SIZE, false);
	if (error != 0)
		goto fail;
	mt7921_wr(sc, MT_WFDMA0_TX_RING_EXT_CTRL(0), 0x4);

	/* Commands to the WM MCU. */
	error = mt7921_ring_alloc(sc, &sc->sc_tx_wm, "tx-mcu",
		MT_TX_RING_BASE, MT7921_TXQ_MCU_WM, MT7921_TX_MCU_NDESC,
		MT7921_TX_BUF_SIZE, false);
	if (error != 0)
		goto fail;

	/* Firmware download. */
	error = mt7921_ring_alloc(sc, &sc->sc_tx_fwdl, "tx-fwdl",
		MT_TX_RING_BASE, MT7921_TXQ_FWDL, MT7921_TX_FWDL_NDESC,
		MT7921_TX_BUF_SIZE, false);
	if (error != 0)
		goto fail;

	/* Events from the WM MCU before firmware download. */
	error = mt7921_ring_alloc(sc, &sc->sc_rx_mcu, "rx-mcu",
		MT_RX_EVENT_RING_BASE, 0, MT7921_RX_MCU_NDESC,
		MT7921_RX_BUF_SIZE, true);
	if (error != 0)
		goto fail;

	/* Events after firmware download (ring 4). */
	error = mt7921_ring_alloc(sc, &sc->sc_rx_wa, "rx-mcu-wa",
		MT_RX_WA_RING_BASE, 0, MT7921_RX_WA_NDESC,
		MT7921_RX_BUF_SIZE, true);
	if (error != 0)
		goto fail;

	/* RX data (ring 2). */
	error = mt7921_ring_alloc(sc, &sc->sc_rx_data, "rx-data",
		MT_RX_DATA_RING_BASE, 0, MT7921_RX_DATA_NDESC,
		MT7921_RX_BUF_SIZE, true);
	if (error != 0)
		goto fail;

	mt7921_rx_fill(sc, &sc->sc_rx_mcu);
	mt7921_rx_fill(sc, &sc->sc_rx_wa);
	mt7921_rx_fill(sc, &sc->sc_rx_data);

	mt7921_dma_enable(sc);
	sc->sc_dma_up = true;

	/* RST_DTX_PTR zeroed the TX DMA indexes; resync our heads. */
	sc->sc_tx_data0.head = 0;
	sc->sc_tx_wm.head = 0;
	sc->sc_tx_fwdl.head = 0;

	device_printf(sc->sc_dev, "DMA rings up (GLO_CFG 0x%08x)\n",
		mt7921_rr(sc, MT_WFDMA0_GLO_CFG));
	return 0;

fail:
	/* The caller frees the memory after turning off bus mastering. */
	mt7921_dma_stop(sc);
	return error;
}


static void
mt7921_ring_unprogram(struct mt7921_softc* sc, struct mt7921_ring* r)
{
	if (r->desc_tag == NULL)
		return;
	mt7921_ring_wr(sc, r, MT_RING_CPU_IDX, 0);
	mt7921_ring_wr(sc, r, MT_RING_NDESC, 0);
	mt7921_ring_wr(sc, r, MT_RING_DESC_BASE, 0);
}


/*
 * Clean up whatever WFDMA state a previous run (or a previous OS) left
 * behind. A warm reboot does not reset the chip, so a ring may still be
 * enabled and pointing at memory that no longer belongs to us. Called
 * before bus mastering is turned on. Uses only MMIO.
 */
void
mt7921_dma_scrub(struct mt7921_softc* sc)
{
	static const uint32_t rings[] = {
		MT_TX_RING_BASE + MT7921_TXQ_BAND0 * MT_RING_SIZE,
		MT_TX_RING_BASE + MT7921_TXQ_FWDL * MT_RING_SIZE,
		MT_TX_RING_BASE + MT7921_TXQ_MCU_WM * MT_RING_SIZE,
		MT_RX_EVENT_RING_BASE,
		MT_RX_DATA_RING_BASE,
		MT_RX_WA_RING_BASE,
	};
	uint32_t glo = mt7921_rr(sc, MT_WFDMA0_GLO_CFG);
	u_int i;

	mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	mt7921_clear(sc, MT_WFDMA0_GLO_CFG,
		MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);
	if (!mt7921_poll_msec(sc, MT_WFDMA0_GLO_CFG,
			MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY,
			0, 100, 1))
		device_printf(sc->sc_dev, "leftover WFDMA stays busy\n");

	for (i = 0; i < nitems(rings); i++) {
		mt7921_wr(sc, rings[i] + MT_RING_CPU_IDX, 0);
		mt7921_wr(sc, rings[i] + MT_RING_NDESC, 0);
		mt7921_wr(sc, rings[i] + MT_RING_DESC_BASE, 0);
	}

	mt7921_clear(sc, MT_WFDMA0_RST,
		MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
	mt7921_set(sc, MT_WFDMA0_RST,
		MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);

	if (glo & (MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN))
		device_printf(sc->sc_dev, "stopped DMA left running by a previous "
			"run (GLO_CFG was 0x%08x)\n", glo);
}


#define MT7921_RINGS(sc) { \
		&(sc)->sc_tx_data0, &(sc)->sc_tx_wm, &(sc)->sc_tx_fwdl, \
		&(sc)->sc_rx_mcu, &(sc)->sc_rx_wa, &(sc)->sc_rx_data, \
	}


/*
 * First half of mt792x_dma_cleanup(): stop the WFDMA engine and unprogram
 * the rings. The memory stays allocated: the caller must turn off PCI bus
 * mastering (so the chip cannot write to host memory at all) before calling
 * mt7921_dma_free_rings().
 */
void
mt7921_dma_stop(struct mt7921_softc* sc)
{
	struct mt7921_ring* rings[] = MT7921_RINGS(sc);
	u_int i;

	mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	mt7921_clear(sc, MT_WFDMA0_GLO_CFG,
		MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN
		| MT_WFDMA0_GLO_CFG_CSR_DISP_BASE_PTR_CHAIN_EN
		| MT_WFDMA0_GLO_CFG_OMIT_TX_INFO | MT_WFDMA0_GLO_CFG_OMIT_RX_INFO
		| MT_WFDMA0_GLO_CFG_OMIT_RX_INFO_PFET2);
	if (!mt7921_poll_msec(sc, MT_WFDMA0_GLO_CFG,
			MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY,
			0, 100, 1))
		device_printf(sc->sc_dev, "WFDMA still busy at cleanup\n");

	mt7921_clear(sc, MT_WFDMA0_RST,
		MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
	mt7921_set(sc, MT_WFDMA0_RST,
		MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);

	for (i = 0; i < nitems(rings); i++)
		mt7921_ring_unprogram(sc, rings[i]);

	sc->sc_dma_up = false;
}


/* Second half: release ring memory. Only once bus mastering is off. */
void
mt7921_dma_free_rings(struct mt7921_softc* sc)
{
	struct mt7921_ring* rings[] = MT7921_RINGS(sc);
	u_int i;

	for (i = 0; i < nitems(rings); i++)
		mt7921_ring_free(rings[i]);
}
