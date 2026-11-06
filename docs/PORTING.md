# mediatekwifi7921 — native Haiku driver for MediaTek MT7921/MT7922 (AMD RZ608/RZ616)

Target hardware: PCI `14c3:0616` (MT7922 / AMD RZ616), subsystem `14c3:061c`.

Approach ("Route B"): a native driver written against the FreeBSD `net80211`
stack that Haiku already ships (`freebsd_wlan`), using MediaTek's mt76 code as
the reference and source of reusable hardware code. No LinuxKPI.

> I checked these files against current `haiku/haiku` master and `openwrt/mt76`
> (Nov 2026). Anything still marked **VERIFY** is unconfirmed.

---

## 1. Which mt76 code to use

Use **`drivers/net/wireless/mediatek/mt76/mt7921/`** (plus the shared mt76 /
mt792x / connac files), from either the Linux tree or `openwrt/mt76`.

- `mt76x2/` is the **wrong family**. It's the older MT7612/MT7602 802.11ac
  chips with a different firmware interface. Ignore it.
- `morrownr/mt76` is an out-of-tree packaging of openwrt/mt76 with install
  scripts. It's handy for reading, but take code from upstream (openwrt/mt76 or
  Linux) so the license headers are clear.
- **License:** openwrt/mt76's LICENSE is 3-Clause BSD, and the files you need
  (`mt7921/*.c`, `mt792x_core.c`, …) carry `SPDX-License-Identifier:
  BSD-3-Clause-Clear`. Both are compatible with Haiku's MIT license. Keep
  MediaTek's copyright headers on anything you copy.

### Files: keep, adapt, or skip

| mt76 file(s) | What it does | Plan |
|---|---|---|
| `mt7921/pci.c` | PCI probe, BAR remap (`fixed_map[]`, `__mt7921_reg_addr`, `mt7921_reg_map_l1`), WFSYS reset | **Port**: needed for milestone 1 |
| `mt7921/pci_mcu.c`, `mt792x_core.c` (`mt792xe_mcu_drv_pmctrl`/`fw_pmctrl`) | Driver/firmware "own" handshake, MCU send over PCIe | **Port** |
| `mt792x_dma.c`, `dma.c`, `dma.h` | WFDMA ring setup, descriptor rings | **Rewrite** onto `bus_dma(9)` + mbufs, keeping the register logic |
| `mt76_connac_mcu.c/.h`, `mt7921/mcu.c/.h` | Firmware download (ROM patch + RAM code), MCU command formats (dev_info, bss_info, sta_rec, hw_scan, keys, …) | **Mostly reuse**: plain struct packing; swap `sk_buff` for a byte buffer |
| `mt7921/mac.c`, `mt76_connac_mac.c`, `mt76_connac2_mac.h` | TX descriptor (TXWI) writing, RX descriptor parsing | **Reuse logic**, convert to mbufs |
| `mt792x_regs.h`, `mt7921/regs.h`, `mt76_connac.h`, `mt792x.h` | Register and struct definitions | **Reuse** (strip Linux types) |
| `mt7921/main.c`, `mac80211.c`, `mt7921/init.c` | Linux mac80211 callbacks and registration | **Rewrite** as net80211 glue (see §3) |
| `eeprom.c`, `util.c`, `agg-rx.c`, `tx.c` | Helpers; RX reordering | Pick pieces as needed (net80211 handles A-MPDU reorder) |
| `debugfs.c`, `testmode.c`, `usb*.c`, `sdio*.c`, `mt792x_acpi_sar.c`, `wed*`, `mt7921/sdio*`, `mt7921/usb.c` | Debug, USB/SDIO variants, ACPI SAR, offload engines | **Skip** |

Avoiding mt76's Linux `page_pool` RX path entirely and using plain mbuf
clusters sidesteps the exact problem that has blocked FreeBSD's LinuxKPI port.

---

## 2. Haiku-side layout

Model the driver on an existing **FreeBSD-stack** WiFi driver in the tree.
`iprowifi4965` (iwn) is the cleanest PCI example, and `realtekwifi` (rtwn)
shows a multi-file driver. Note: `idualwifi7260` and `iaxwifi200` now build
against the **OpenBSD** stack (`openbsd_wlan.a`, `OBSD_WLAN`), so don't copy
their glue.

**Stack choice.** Haiku carries both FreeBSD net80211 (`freebsd_wlan`) and
OpenBSD net80211 (`openbsd_wlan`). This plan targets FreeBSD's, because
FreeBSD's `linux_80211.c` already maps mac80211 onto it, and it has a
scan-offload hook. OpenBSD's stack is the one Haiku's 802.11ac Intel driver
uses, so it's a reasonable alternative. Ask waddlesplash which he'd prefer
before going past milestone 3. Milestones 1–3 don't depend on the choice.

```
src/add-ons/kernel/drivers/network/wlan/mediatekwifi7921/
├── Jamfile
├── glue.c                 # Haiku glue macros, firmware name map, IRQ check
└── dev/mt7921/
    ├── if_mt7921.c        # newbus probe/attach/detach + net80211 glue
    ├── if_mt7921var.h     # softc
    ├── mt7921_reg.h       # registers (from mt792x_regs.h / mt7921/regs.h)
    ├── mt7921_dma.c       # rings (milestone 2)
    ├── mt7921_mcu.c       # firmware download + MCU commands (milestone 3)
    └── mt7921_mac.c       # TX/RX descriptors (milestone 6)
```

Also needed:
- Add `SubInclude … mediatekwifi7921 ;` to `wlan/Jamfile`.
- Add the driver to the image/package lists (search `build/jam/` for
  `idualwifi7260` and mirror each place it appears).

### Firmware

MT7922 needs two blobs from linux-firmware's `mediatek/` directory:

- `WIFI_MT7922_patch_mcu_1_1_hdr.bin` (ROM patch)
- `WIFI_RAM_CODE_MT7922_1.bin` (WM RAM code)

(MT7921 uses `WIFI_MT7961_patch_mcu_1_2_hdr.bin` and `WIFI_RAM_CODE_MT7961_1.bin`.)

The compat layer's `firmware_get()` (`src/libs/compat/freebsd_network/firmware.c`)
maps names through `HAIKU_FIRMWARE_NAME_MAP`, then tries, in order:

1. `/boot/system/non-packaged/data/firmware/mediatekwifi7921/<file>`
2. `/boot/system/data/firmware/mediatekwifi7921/<file>`

For development, just copy the blobs into the first (writable) path.

`install-wifi-firmwares.sh` only covers firmware Haiku **can't**
redistribute, which needs a license click-through (ipw2100/2200, b43, malo). As
far as I know, MediaTek's linux-firmware license (`LICENCE.mediatek`) allows
binary redistribution. If so, the blobs belong in the `haiku/firmware` repo and
a normal package, not in that script. **VERIFY** the license text before
proposing it upstream.

---

## 3. mac80211 → net80211 mapping

mt76 is written for Linux mac80211. You're replacing that layer by hand. For
the best reference on how each mac80211 callback maps onto net80211, read
FreeBSD's `sys/compat/linuxkpi/common/src/linux_80211.c`. It solves exactly this
translation generically, and it runs mt7921 today.

| mt7921 `main.c` op | net80211 equivalent |
|---|---|
| `start` / `stop` | `ic_parent` (up/down) |
| `add_interface` / `remove_interface` | `ic_vap_create` / `ic_vap_delete` |
| `config` (channel change) | `ic_set_channel` |
| `hw_scan` / `cancel_hw_scan` | `IEEE80211_FEXT_SCAN_OFFLOAD` + `ic_scan_start` / `ic_scan_end` / `ic_scan_curchan`; feed beacons/probe responses up with `ieee80211_input_mimo_all` / `ieee80211_add_scan` |
| `sta_state`, `bss_info_changed` | `iv_newstate` (AUTH/ASSOC/RUN transitions send dev_info, bss_info, sta_rec MCU cmds) |
| `set_key` | `iv_key_alloc` / `iv_key_set` / `iv_key_delete` (start with **software crypto**) |
| `tx` / `wake_tx_queue` | `ic_transmit` + `ic_raw_xmit` |
| `conf_tx` (WMM params) | `ic_wme.wme_update` |
| `ampdu_action` | `ic_ampdu_rx_start/stop`, `ic_addba_request/response` |
| `get_stats`, `sta_statistics` | `ic_getradiocaps`, node stats |

Linux primitives to swap:

| Linux | FreeBSD/Haiku compat |
|---|---|
| `sk_buff` | `mbuf` |
| `dma_alloc_coherent`, `dma_map_single` | `bus_dma_tag_create`, `bus_dmamem_alloc`, `bus_dmamap_load` |
| `napi` | interrupt filter + taskqueue |
| `work_struct`, `delayed_work` | `taskqueue_enqueue`, `callout` / `taskqueue_enqueue_timeout` |
| `mutex`, `spinlock` | `mtx` |
| `wait_event_timeout`, `completion` (MCU response wait) | `msleep` / `wakeup` on the sequence number |
| `request_firmware` | `firmware_get` / `firmware_put` |
| `readl` / `writel` | `bus_read_4` / `bus_write_4` via the remap function |

### 802.11ax / 6 GHz caveat

net80211 has no HE (802.11ax) support, so you won't get Wi-Fi 6 rates or
6 GHz at first. Advertise HT/VHT in `sta_rec`; the firmware handles rate
control itself. Treat HE/6 GHz as a late stretch goal.

---

## 4. Milestones

Each milestone produces something you can see in `syslog` or in the UI.

1. **Attach and talk to the chip.** Match `14c3:0616`, map BAR0, port the
   address-remap table, take "driver own", read the chip ID and revision, and
   `firmware_get()` both blobs. *Done when syslog prints `ASIC revision:
   7922xxxx` and both firmware sizes.* ← **the skeleton in this directory
   implements this.** It returns ENXIO at the end on purpose, so no broken
   interface appears.
   ✅ Done (Nov 2026).
2. **DMA rings** (`mt7921_dma.c`). WFDMA0 reset/init, MCU TX rings (WM +
   FWDL), RX event rings, all **polled** with host interrupts masked.
   Interrupt handling moves to milestone 4, together with the data path.
   ✅ Done (Nov 2026).
3. **Firmware boot** (`mt7921_mcu.c`). Patch semaphore → load ROM patch → load RAM code
   regions → FW start → poll `MT_CONN_ON_MISC` for `MT_TOP_MISC2_FW_N9_RDY`. Then the
   first real MCU command, `get_nic_capability`, which returns the MAC address.
   *Done when syslog prints the card's real MAC.* ✅ Done (Nov 2026).
4. **net80211 attach + interrupts** (`mt7921_net.c`, stage 4). MSI-only
   interrupt thread (MCU events logged), channel list, `ieee80211_ifattach`,
   VAP create, radio hooks stubbed. DMA is paused and bus mastering off
   whenever the interface is down, because Haiku has no driver shutdown hook
   and a warm reboot does not reset the chip. *Done when the interface
   appears in Network preferences with the right MAC.* ✅ Done (Nov 2026).
   Open issue: Haiku has no shutdown hook, and taking the interface down
   does not happen at reboot, so DMA can stay live across a warm reboot.
   The startup scrub catches it; the driver logs the PCI command register
   at load to show whether the BIOS cleared bus mastering in between.
5. **Scanning** (stage 5). Generic MCU sender (CMD/EXT/CE/UNI), persistent
   lock-protected MCU context, MAC init (EFUSE mode, MAC registers, MAC
   enable, channel domain, RX path, DEV_INFO + BSS_INFO), RX descriptor
   parsing for both the data ring and MCU-forwarded frames, delivery to
   net80211 from a task, firmware hw_scan with `IEEE80211_FEXT_SCAN_OFFLOAD`
   and SCAN_DONE handling. *Done when Haiku's network menu lists nearby
   networks.* Not yet: CLC/regulatory, TX power tables (firmware defaults).
   ✅ Done (Nov 2026): beacons/probe responses received, networks listed.
6. **Transmit + joining** (stage 6). Data ring with TXD + TXP slots (slot =
   MSDU token, freed by TX-free events), fixed basic rate for management /
   group frames, firmware rate control for unicast data. Joining: STA_REC
   (basic/phy/ra/state + WTBL generic/rx/hdr_trans) and ROC before AUTH;
   BSS_INFO basic+qos, RLM, STA_REC(ASSOC) on RUN; removal on leave.
   Legacy rates only (no HT/VHT/HE, no WME/QoS) for now. *Done when DHCP
   gets an address and you can ping.*
7. **WPA2.** Software CCMP in net80211 (built into Haiku's freebsd_wlan);
   the hardware has no keys and passes frames through. Hardware key offload
   later. FreeBSD hit a bug where enabling HW crypto broke TX, so keep a toggle.
8. **Polish.** HT/VHT capabilities, A-MPDU, power save, clean detach (FreeBSD's
   port also panicked on unload; test `detach` early and often), suspend/resume.

---

### Bring-up stages (safety switch)

Attach runs stage by stage and stops after the stage set in
`/boot/home/config/settings/kernel/drivers/mediatekwifi7921`
(`stage 1` … `stage 6`; default: all). After the last stage it
stops DMA, resets the WiFi core, masks interrupts, hands the chip back to
the firmware and declines to attach.

## 5. Dev setup

- Install a current Haiku nightly on this machine. Keep a USB Realtek dongle
  or Ethernet for network access while you work.
- Build Haiku (or just this add-on with `jam -q mediatekwifi7921`) either on
  Haiku itself or with a cross toolchain.
- Copy the firmware: `WIFI_MT7922_patch_mcu_1_1_hdr.bin` and
  `WIFI_RAM_CODE_MT7922_1.bin` into
  `/boot/system/non-packaged/data/firmware/mediatekwifi7921/`.
- Install the built driver to
  `/boot/system/non-packaged/add-ons/kernel/drivers/bin/` and symlink from
  `…/drivers/dev/net/`. Restart `net_server`, or reboot, to rescan.
- Log with `dprintf` / `device_printf` and read `/var/log/syslog`. Enable
  on-screen KDL or serial debugging, since early DMA bugs tend to hang the machine.
- Keep a Linux live USB around: `mt7921e` with `dyndbg` and `trace-cmd` lets you
  capture the exact MCU command sequence Linux sends, to diff against yours.

## 6. Where to get help

- Haiku forum / `haiku-development` list. Ask early whether a native MediaTek
  driver would be accepted upstream, and how firmware should be packaged.
- FreeBSD `freebsd-wireless` list. bz@ has the most hands-on mt7921 bring-up
  experience outside Linux.
