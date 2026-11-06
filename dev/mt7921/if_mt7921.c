/*
 * Copyright 2026, Techniix. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Portions derived from MediaTek's mt76 driver (mt7921/pci.c, mt792x_core.c,
 * mt792x_dma.c, util.c):
 *   Copyright (C) 2020-2023 MediaTek Inc.
 *   SPDX-License-Identifier: BSD-3-Clause-Clear
 *
 * Native driver for MediaTek MT7921/MT7922 (AMD RZ608/RZ616) PCIe WiFi,
 * written against FreeBSD net80211 via Haiku's freebsd_wlan compat layer.
 *
 * Bring-up stages (see MT7921_STAGE_* and glue.c for the settings file):
 *   1: map BAR0, ownership handshake, read chip ID, reset WF subsystem,
 *      check firmware files.
 *   2: + set up and enable the WFDMA rings (polled, interrupts masked).
 *   3: + download and boot the firmware, read the MAC address.
 *   4: + enable interrupts and register with net80211; the driver stays
 *      attached and Haiku creates a (not yet functional) WiFi interface.
 *   5: + MAC/radio setup in firmware, receive path, firmware scanning.
 *   6: + transmit path, joining open and WPA2 networks.
 * Below stage 4 the driver tears everything down again and declines to
 * attach (ENXIO).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/firmware.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>

#include <dev/mt7921/if_mt7921var.h>
#include <dev/mt7921/mt7921_reg.h>


/* Names passed to firmware_get(); glue.c maps them to files on disk. */
#define MT7921_FW_PATCH	"mt7921_patch"
#define MT7921_FW_RAM	"mt7921_ram"
#define MT7922_FW_PATCH	"mt7922_patch"
#define MT7922_FW_RAM	"mt7922_ram"

/* From mt76/mt7921/pci.c mt7921_pci_device_table[]. */
static const struct mt7921_ident mt7921_ident_table[] = {
	{ 0x14c3, 0x7961, MT7921_CHIP_7921, "MediaTek MT7921 802.11ax" },
	{ 0x14c3, 0x7922, MT7921_CHIP_7922, "MediaTek MT7922 802.11ax" },
	{ 0x0b48, 0x7922, MT7921_CHIP_7922, "MediaTek MT7922 802.11ax" },
	{ 0x14c3, 0x0608, MT7921_CHIP_7921, "AMD RZ608 (MT7921) 802.11ax" },
	{ 0x14c3, 0x0616, MT7921_CHIP_7922, "AMD RZ616 (MT7922) 802.11ax" },
	/* MT7920 and MT7902 use different firmware; add them once 7921/7922 work. */
	{ 0, 0, 0, NULL }
};


/* #pragma mark - register access */


static inline uint32_t
mt7921_bar_rr(struct mt7921_softc* sc, uint32_t ofs)
{
	return bus_read_4(sc->sc_mem, ofs);
}


static inline void
mt7921_bar_wr(struct mt7921_softc* sc, uint32_t ofs, uint32_t val)
{
	bus_write_4(sc->sc_mem, ofs, val);
}


/* Port of mt7921_reg_map_l1() (mt76/mt7921/mt7921.h). Returns a BAR offset. */
static uint32_t
mt7921_reg_map_l1(struct mt7921_softc* sc, uint32_t addr)
{
	uint32_t offset = addr & MT_HIF_REMAP_L1_OFFSET;
	uint32_t base = (addr & MT_HIF_REMAP_L1_BASE) >> 16;
	uint32_t v;

	v = mt7921_bar_rr(sc, MT_HIF_REMAP_L1);
	v = (v & ~MT_HIF_REMAP_L1_MASK) | (base & MT_HIF_REMAP_L1_MASK);
	mt7921_bar_wr(sc, MT_HIF_REMAP_L1, v);
	/* Use a read to push the write. */
	(void)mt7921_bar_rr(sc, MT_HIF_REMAP_L1);

	return MT_HIF_REMAP_BASE_L1 + offset;
}


/*
 * Port of __mt7921_reg_addr() (mt76/mt7921/pci.c): translate a chip address
 * to a BAR0 offset. Returns 0xffffffff for unsupported addresses.
 */
static uint32_t
mt7921_reg_addr(struct mt7921_softc* sc, uint32_t addr)
{
	u_int i;

	if (addr < 0x100000)
		return addr;

	for (i = 0; i < nitems(mt7921_fixed_map); i++) {
		uint32_t ofs;

		if (addr < mt7921_fixed_map[i].phys)
			continue;

		ofs = addr - mt7921_fixed_map[i].phys;
		/* Upstream uses '>' here, not '>='; kept identical on purpose. */
		if (ofs > mt7921_fixed_map[i].size)
			continue;

		return mt7921_fixed_map[i].maps + ofs;
	}

	if ((addr >= 0x18000000 && addr < 0x18c00000)
		|| (addr >= 0x70000000 && addr < 0x78000000)
		|| (addr >= 0x7c000000 && addr < 0x7c400000))
		return mt7921_reg_map_l1(sc, addr);

	device_printf(sc->sc_dev, "unsupported register address 0x%08x\n", addr);
	return 0xffffffff;
}


uint32_t
mt7921_rr(struct mt7921_softc* sc, uint32_t addr)
{
	uint32_t ofs = mt7921_reg_addr(sc, addr);
	if (ofs == 0xffffffff)
		return 0xffffffff;
	return mt7921_bar_rr(sc, ofs);
}


void
mt7921_wr(struct mt7921_softc* sc, uint32_t addr, uint32_t val)
{
	uint32_t ofs = mt7921_reg_addr(sc, addr);
	if (ofs == 0xffffffff)
		return;
	mt7921_bar_wr(sc, ofs, val);
}


uint32_t
mt7921_rmw(struct mt7921_softc* sc, uint32_t addr, uint32_t mask, uint32_t val)
{
	uint32_t cur = mt7921_rr(sc, addr);
	val |= cur & ~mask;
	mt7921_wr(sc, addr, val);
	return val;
}


/* mt7921_l1_rr(): force an access through the L1 window. */
uint32_t
mt7921_l1_rr(struct mt7921_softc* sc, uint32_t addr)
{
	return mt7921_bar_rr(sc, mt7921_reg_map_l1(sc, addr));
}


uint32_t
mt7921_l1_map(struct mt7921_softc* sc, uint32_t addr)
{
	return mt7921_reg_map_l1(sc, addr);
}


/* Port of mt7921_mac_wtbl_update(idx, MT_WTBL_UPDATE_ADM_COUNT_CLEAR). */
bool
mt7921_wtbl_update(struct mt7921_softc* sc, int idx)
{
	mt7921_rmw(sc, MT_WTBL_UPDATE, MT_WTBL_UPDATE_WLAN_IDX,
		((uint32_t)idx & MT_WTBL_UPDATE_WLAN_IDX)
		| MT_WTBL_UPDATE_ADM_COUNT_CLEAR);
	return mt7921_poll_msec(sc, MT_WTBL_UPDATE, MT_WTBL_UPDATE_BUSY, 0, 5, 1);
}


/* Port of ____mt76_poll_msec() (mt76/util.c). */
bool
mt7921_poll_msec(struct mt7921_softc* sc, uint32_t addr, uint32_t mask,
	uint32_t val, int timeout_ms, int tick_ms)
{
	int n = timeout_ms / tick_ms;

	do {
		if ((mt7921_rr(sc, addr) & mask) == val)
			return true;
		DELAY(1000 * tick_ms);
	} while (n-- > 0);

	return false;
}


/* #pragma mark - power ownership */


/* Port of mt792xe_mcu_fw_pmctrl(): hand the chip to the firmware. */
static int
mt7921_fw_own(struct mt7921_softc* sc)
{
	int i;

	for (i = 0; i < MT792X_DRV_OWN_RETRY_COUNT; i++) {
		mt7921_wr(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_SET_OWN);
		if (mt7921_poll_msec(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC,
				PCIE_LPCR_HOST_OWN_SYNC, 50, 1))
			return 0;
	}

	device_printf(sc->sc_dev, "firmware own failed\n");
	return EIO;
}


/* Port of __mt792xe_mcu_drv_pmctrl(): take the chip back for the driver. */
static int
mt7921_drv_own(struct mt7921_softc* sc)
{
	int i;

	for (i = 0; i < MT792X_DRV_OWN_RETRY_COUNT; i++) {
		mt7921_wr(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);
		/* Upstream sleeps 2-3 ms here when ASPM is enabled. */
		DELAY(3000);
		if (mt7921_poll_msec(sc, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC,
				0, 50, 1))
			return 0;
	}

	device_printf(sc->sc_dev, "driver own failed\n");
	return EIO;
}


/* Port of mt792x_wfsys_reset_default() for connac2 chips. */
static int
mt7921_wfsys_reset(struct mt7921_softc* sc)
{
	mt7921_clear(sc, MT_WFSYS_SW_RST, WFSYS_SW_RST_B);
	DELAY(50 * 1000);
	mt7921_set(sc, MT_WFSYS_SW_RST, WFSYS_SW_RST_B);

	if (!mt7921_poll_msec(sc, MT_WFSYS_SW_RST, WFSYS_SW_INIT_DONE,
			WFSYS_SW_INIT_DONE, 500, 1)) {
		device_printf(sc->sc_dev, "WF subsystem reset timed out\n");
		return ETIMEDOUT;
	}
	return 0;
}


/*
 * Leave the chip quiet: mask and ack every interrupt source, then hand it
 * back to the firmware so it can go to sleep. Must run on every exit path
 * once BAR0 is mapped, or a pending interrupt can scream on a shared IRQ.
 */
static void
mt7921_quiesce(struct mt7921_softc* sc)
{
	uint32_t sta;

	mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	mt7921_wr(sc, MT_PCIE_MAC_INT_ENABLE, 0);
	sta = mt7921_rr(sc, MT_WFDMA0_HOST_INT_STA);
	if (sta != 0 && sta != 0xffffffff)
		mt7921_wr(sc, MT_WFDMA0_HOST_INT_STA, sta);

	if (mt7921_fw_own(sc) == 0)
		device_printf(sc->sc_dev, "interrupts masked, chip handed back to "
			"firmware\n");
}


static void
mt7921_disable_busmaster(device_t dev)
{
	uint16_t cmd = pci_read_config(dev, PCIR_COMMAND, 2);
	pci_write_config(dev, PCIR_COMMAND, cmd & ~PCIM_CMD_BUSMASTEREN, 2);
}


/* #pragma mark - firmware */


static int
mt7921_check_firmware(struct mt7921_softc* sc, const char* name)
{
	const struct firmware* fw = firmware_get(name);
	if (fw == NULL) {
		device_printf(sc->sc_dev, "could not load firmware '%s'\n", name);
		return ENOENT;
	}
	device_printf(sc->sc_dev, "firmware '%s': %zu bytes\n", name,
		(size_t)fw->datasize);
	firmware_put(fw, FIRMWARE_UNLOAD);
	return 0;
}


/* #pragma mark - newbus */


static const struct mt7921_ident*
mt7921_lookup(device_t dev)
{
	const struct mt7921_ident* id;

	for (id = mt7921_ident_table; id->name != NULL; id++) {
		if (pci_get_vendor(dev) == id->vendor
			&& pci_get_device(dev) == id->device)
			return id;
	}
	return NULL;
}


static int
mt7921_probe(device_t dev)
{
	const struct mt7921_ident* id = mt7921_lookup(dev);
	if (id == NULL)
		return ENXIO;

	device_set_desc(dev, id->name);
	return BUS_PROBE_DEFAULT;
}


/* #pragma mark - interrupts */


/* WFDMA host interrupt sources we handle (mt7921/regs.h). */
#define MT_INT_RX_DONE_WM	BIT(0)	/* MCU events, before firmware */
#define MT_INT_RX_DONE_DATA	BIT(2)	/* RX data ring */
#define MT_INT_RX_DONE_WM2	BIT(22)	/* MCU events, after firmware */
#define MT_INT_MCU_CMD		BIT(29)	/* MCU-to-host software interrupt */
#define MT7921_INT_MASK		(MT_INT_RX_DONE_WM | MT_INT_RX_DONE_DATA \
				| MT_INT_RX_DONE_WM2 | MT_INT_MCU_CMD)


/*
 * Interrupt thread. The hard-IRQ half (HAIKU_CHECK_DISABLE_INTERRUPTS in
 * glue.c) has already masked all host interrupts. Port of
 * mt792x_irq_tasklet(), without TX completion (TX is still synchronous).
 */
void
mt7921_intr(void* arg)
{
	struct mt7921_softc* sc = arg;
	uint32_t intr;

	if (!sc->sc_intr_ready)
		return;

	MT7921_LOCK(sc);
	intr = mt7921_rr(sc, MT_WFDMA0_HOST_INT_STA) & sc->sc_irqmask;
	mt7921_wr(sc, MT_WFDMA0_HOST_INT_STA, intr);

	if (sc->sc_intr_logged < 6) {
		sc->sc_intr_logged++;
		device_printf(sc->sc_dev, "interrupt 0x%08x\n", intr);
	}

	if (intr & MT_INT_MCU_CMD) {
		uint32_t sw = mt7921_rr(sc, MT_MCU_CMD);
		mt7921_wr(sc, MT_MCU_CMD, sw);	/* ack MCU2HOST_SW_INT_STA */
	}

	/* MCU replies/events and received frames, from all RX rings. */
	mt7921_mcu_poll_locked(sc);
	MT7921_UNLOCK(sc);

	if (sc->sc_intr_ready)
		mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, sc->sc_irqmask);
}


/*
 * MSI only: a private vector cannot disturb other devices the way a shared
 * legacy interrupt line can. Without MSI the driver runs without
 * interrupts, which is fine while nothing depends on them yet.
 */
static void
mt7921_intr_setup(struct mt7921_softc* sc)
{
	device_t dev = sc->sc_dev;
	uint32_t sta;
	int count = 1, error;

	if (pci_alloc_msi(dev, &count) != 0 || count != 1) {
		device_printf(dev, "MSI not available; running without "
			"interrupts\n");
		return;
	}

	sc->sc_irq_rid = 1;
	sc->sc_irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->sc_irq_rid,
		RF_ACTIVE);
	if (sc->sc_irq == NULL) {
		device_printf(dev, "can't allocate MSI; running without "
			"interrupts\n");
		pci_release_msi(dev);
		sc->sc_irq_rid = 0;
		return;
	}

	error = bus_setup_intr(dev, sc->sc_irq, INTR_TYPE_NET | INTR_MPSAFE,
		NULL, mt7921_intr, sc, &sc->sc_ih);
	if (error != 0) {
		device_printf(dev, "can't set up interrupt (%d); running without "
			"interrupts\n", error);
		sc->sc_ih = NULL;
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_irq_rid, sc->sc_irq);
		pci_release_msi(dev);
		sc->sc_irq = NULL;
		sc->sc_irq_rid = 0;
		return;
	}

	/* Clear anything pending, then unmask. */
	sta = mt7921_rr(sc, MT_WFDMA0_HOST_INT_STA);
	mt7921_wr(sc, MT_WFDMA0_HOST_INT_STA, sta);
	sc->sc_irqmask = MT7921_INT_MASK;
	sc->sc_intr_ready = true;
	mt7921_wr(sc, MT_PCIE_MAC_INT_ENABLE, 0xff);
	mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, sc->sc_irqmask);

	device_printf(dev, "interrupts enabled (MSI)\n");
}


/*
 * Haiku has no driver shutdown hook, and a warm reboot does not reset this
 * chip. So the chip may only reach host memory while the interface is up:
 * pause DMA and drop bus mastering whenever it goes down (and right after
 * attach), resume when net80211 brings it up.
 */
void
mt7921_hw_pause(struct mt7921_softc* sc)
{
	MT7921_LOCK(sc);
	if (sc->sc_dma_up && !sc->sc_dma_paused) {
		mt7921_clear(sc, MT_WFDMA0_GLO_CFG,
			MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);
		if (!mt7921_poll_msec(sc, MT_WFDMA0_GLO_CFG,
				MT_WFDMA0_GLO_CFG_TX_DMA_BUSY | MT_WFDMA0_GLO_CFG_RX_DMA_BUSY,
				0, 100, 1))
			device_printf(sc->sc_dev, "WFDMA busy while pausing\n");
		mt7921_disable_busmaster(sc->sc_dev);
		sc->sc_dma_paused = true;
		device_printf(sc->sc_dev, "DMA paused, bus mastering off\n");
	}
	MT7921_UNLOCK(sc);
}


void
mt7921_hw_resume(struct mt7921_softc* sc)
{
	MT7921_LOCK(sc);
	if (sc->sc_dma_up && sc->sc_dma_paused) {
		pci_enable_busmaster(sc->sc_dev);
		mt7921_set(sc, MT_WFDMA0_GLO_CFG,
			MT_WFDMA0_GLO_CFG_TX_DMA_EN | MT_WFDMA0_GLO_CFG_RX_DMA_EN);
		sc->sc_dma_paused = false;
		device_printf(sc->sc_dev, "DMA resumed\n");
	}
	MT7921_UNLOCK(sc);
}


static void
mt7921_intr_teardown(struct mt7921_softc* sc)
{
	sc->sc_intr_ready = false;
	if (sc->sc_mem != NULL) {
		mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, 0);
		mt7921_wr(sc, MT_PCIE_MAC_INT_ENABLE, 0);
	}
	if (sc->sc_ih != NULL) {
		/* Waits for the interrupt thread to finish. */
		bus_teardown_intr(sc->sc_dev, sc->sc_irq, sc->sc_ih);
		sc->sc_ih = NULL;
	}
}


static void
mt7921_release(struct mt7921_softc* sc)
{
	if (sc->sc_ih != NULL)
		bus_teardown_intr(sc->sc_dev, sc->sc_irq, sc->sc_ih);
	if (sc->sc_irq != NULL) {
		bus_release_resource(sc->sc_dev, SYS_RES_IRQ, sc->sc_irq_rid,
			sc->sc_irq);
		if (sc->sc_irq_rid != 0)
			pci_release_msi(sc->sc_dev);
	}
	if (sc->sc_mem != NULL)
		bus_release_resource(sc->sc_dev, SYS_RES_MEMORY, sc->sc_mem_rid,
			sc->sc_mem);
	sc->sc_ih = NULL;
	sc->sc_irq = NULL;
	sc->sc_mem = NULL;
}


static int
mt7921_attach(device_t dev)
{
	struct mt7921_softc* sc = device_get_softc(dev);
	const struct mt7921_ident* id = mt7921_lookup(dev);
	uint32_t chipid, rev;
	bool dma_touched = false;
	int error;

	sc->sc_dev = dev;
	sc->sc_chip = id->chip;
	sc->sc_stage = mt7921_haiku_get_stage();
	MT7921_LOCK_INIT(sc);
	device_printf(dev, "bring-up stage %d of %d\n", sc->sc_stage,
		MT7921_STAGE_MAX);

	if (sc->sc_chip == MT7921_CHIP_7922) {
		sc->sc_fw_patch = MT7922_FW_PATCH;
		sc->sc_fw_ram = MT7922_FW_RAM;
	} else {
		sc->sc_fw_patch = MT7921_FW_PATCH;
		sc->sc_fw_ram = MT7921_FW_RAM;
	}

	/*
	 * Enable memory decoding only. Bus mastering (the chip's ability to
	 * read and write host memory) stays off until DMA has been scrubbed
	 * and our own rings exist: a warm reboot does not reset the chip, so
	 * it may still hold ring addresses from a previous run.
	 */
	{
		/*
		 * Diagnostic: after a warm reboot, is bus mastering still on?
		 * If the BIOS left it on, a chip with live DMA rings could have
		 * written into memory before we got here.
		 */
		uint16_t cmd = pci_read_config(dev, PCIR_COMMAND, 2);
		device_printf(dev, "PCI command at load: 0x%04x (bus master %s)\n",
			cmd, (cmd & PCIM_CMD_BUSMASTEREN) ? "ON" : "off");
	}
	if (pci_get_powerstate(dev) != PCI_POWERSTATE_D0)
		pci_set_powerstate(dev, PCI_POWERSTATE_D0);
	mt7921_disable_busmaster(dev);
	pci_enable_io(dev, SYS_RES_MEMORY);

	sc->sc_mem_rid = PCIR_BAR(0);
	sc->sc_mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->sc_mem_rid,
		RF_ACTIVE);
	if (sc->sc_mem == NULL) {
		device_printf(dev, "can't map BAR0\n");
		error = ENXIO;
		goto fail;
	}
	device_printf(dev, "BAR0 mapped, size 0x%lx\n",
		(unsigned long)rman_get_size(sc->sc_mem));

	/*
	 * Same order as mt7921_pci_probe(): hand the chip to the firmware,
	 * then take it back, which leaves it awake and owned by the driver.
	 */
	if ((error = mt7921_fw_own(sc)) != 0)
		goto fail;
	if ((error = mt7921_drv_own(sc)) != 0)
		goto fail;

	/* Mask all interrupts right away; nothing here handles them yet. */
	mt7921_wr(sc, MT_WFDMA0_HOST_INT_ENA, 0);
	mt7921_wr(sc, MT_PCIE_MAC_INT_ENABLE, 0);

	/* Stop any DMA a previous run left behind. */
	mt7921_dma_scrub(sc);

	chipid = mt7921_l1_rr(sc, MT_HW_CHIPID) & 0xffff;
	if (chipid == 0x7961 && (mt7921_l1_rr(sc, MT_HW_BOUND) & BIT(7)))
		chipid = 0x7920;
	rev = mt7921_l1_rr(sc, MT_HW_REV) & 0xff;
	sc->sc_rev = (chipid << 16) | rev;
	device_printf(dev, "ASIC revision: %08x (chip 0x%04x rev 0x%02x)\n",
		sc->sc_rev, chipid, rev);

	if (chipid != 0x7961 && chipid != 0x7922) {
		device_printf(dev, "unexpected chip id; bailing out\n");
		error = ENXIO;
		goto fail;
	}

	if ((error = mt7921_wfsys_reset(sc)) != 0)
		goto fail;
	device_printf(dev, "WF subsystem reset OK\n");

	if ((error = mt7921_check_firmware(sc, sc->sc_fw_patch)) != 0)
		goto fail;
	if ((error = mt7921_check_firmware(sc, sc->sc_fw_ram)) != 0)
		goto fail;

	if (sc->sc_stage >= MT7921_STAGE_DMA) {
		dma_touched = true;
		/* Only now may the chip access host memory. */
		pci_enable_busmaster(dev);
		if ((error = mt7921_dma_init(sc)) != 0) {
			device_printf(dev, "DMA setup failed (%d)\n", error);
			goto fail;
		}
	}

	if (sc->sc_stage >= MT7921_STAGE_FIRMWARE) {
		if ((error = mt7921_mcu_alloc(sc)) != 0)
			goto fail;
		if ((error = mt7921_mcu_run_firmware(sc)) != 0) {
			device_printf(dev, "firmware bring-up failed (%d)\n", error);
			goto fail;
		}
	}

	if (sc->sc_stage >= MT7921_STAGE_SCAN) {
		if ((error = mt7921_mcu_init_mac(sc)) != 0) {
			device_printf(dev, "MAC/radio setup failed (%d)\n", error);
			goto fail;
		}
		sc->sc_radio_ready = true;
		sc->sc_can_tx = sc->sc_stage >= MT7921_STAGE_CONNECT;
	}

	if (sc->sc_stage >= MT7921_STAGE_NET80211) {
		if ((error = mt7921_net_attach(sc)) != 0) {
			device_printf(dev, "net80211 attach failed (%d)\n", error);
			goto fail;
		}
		mt7921_intr_setup(sc);
		/* Until the interface is brought up, the chip gets no memory access. */
		mt7921_hw_pause(sc);
		device_printf(dev, "attached; Haiku will now create the WiFi "
			"interface\n");
		return 0;
	}

	device_printf(dev, "stage %d complete; not attaching yet\n",
		sc->sc_stage);
	error = ENXIO;

fail:
	mt7921_intr_teardown(sc);
	mt7921_net_detach(sc);

	/*
	 * Teardown order matters: stop DMA, then cut off the chip's access to
	 * host memory (bus mastering), then reset the WiFi core, and only then
	 * free the memory the chip used. Resetting the core restarts its ROM
	 * code, which must not be able to write into memory we gave back.
	 */
	if (dma_touched)
		mt7921_dma_stop(sc);
	mt7921_disable_busmaster(dev);
	if (dma_touched)
		(void)mt7921_wfsys_reset(sc);
	if (sc->sc_mem != NULL)
		mt7921_quiesce(sc);
	if (dma_touched) {
		mt7921_dma_free_rings(sc);
		device_printf(dev, "DMA stopped, bus mastering off, memory freed\n");
	}
	mt7921_mcu_free(sc);
	mt7921_release(sc);
	MT7921_LOCK_DESTROY(sc);
	return error;
}


static int
mt7921_detach(device_t dev)
{
	struct mt7921_softc* sc = device_get_softc(dev);

	/* Haiku's stop_wlan() has already deleted the VAP at this point. */
	mt7921_intr_teardown(sc);
	mt7921_net_detach(sc);

	/* Same order as the attach failure path. */
	mt7921_dma_stop(sc);
	mt7921_disable_busmaster(dev);
	(void)mt7921_wfsys_reset(sc);
	if (sc->sc_mem != NULL)
		mt7921_quiesce(sc);
	mt7921_dma_free_rings(sc);
	mt7921_mcu_free(sc);
	mt7921_release(sc);
	MT7921_LOCK_DESTROY(sc);
	device_printf(dev, "detached\n");
	return 0;
}


static device_method_t mt7921_methods[] = {
	DEVMETHOD(device_probe,		mt7921_probe),
	DEVMETHOD(device_attach,	mt7921_attach),
	DEVMETHOD(device_detach,	mt7921_detach),
	DEVMETHOD_END
};

static driver_t mt7921_driver = {
	"mt7921",
	mt7921_methods,
	sizeof(struct mt7921_softc)
};

DRIVER_MODULE(mt7921, pci, mt7921_driver, NULL, NULL);
MODULE_VERSION(mt7921, 1);
MODULE_DEPEND(mt7921, firmware, 1, 1, 1);
MODULE_DEPEND(mt7921, pci, 1, 1, 1);
MODULE_DEPEND(mt7921, wlan, 1, 1, 1);
