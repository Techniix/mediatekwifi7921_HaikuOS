# mediatekwifi7921 — MediaTek MT7921/MT7922 WiFi for Haiku

A native [Haiku](https://www.haiku-os.org/) driver for MediaTek MT7921 and
MT7922 PCIe WiFi cards (sold by AMD as **RZ608** and **RZ616**), ported from
MediaTek's Linux [mt76](https://github.com/openwrt/mt76) driver onto the
FreeBSD `net80211` WiFi stack that Haiku ships.

> **Status: experimental.** It scans, joins open and WPA2 networks, and
> carries real traffic on an AMD RZ616 (MT7922). It has not had much testing
> beyond that. Keep a wired connection or a supported USB WiFi adapter handy.

## Supported hardware

| Card | PCI ID | Firmware |
|---|---|---|
| MediaTek MT7922 / **AMD RZ616** | `14c3:0616`, `14c3:7922`, `0b48:7922` | `WIFI_MT7922_patch_mcu_1_1_hdr.bin`, `WIFI_RAM_CODE_MT7922_1.bin` |
| MediaTek MT7921 / **AMD RZ608** | `14c3:0608`, `14c3:7961` | `WIFI_MT7961_patch_mcu_1_2_hdr.bin`, `WIFI_RAM_CODE_MT7961_1.bin` |

Tested: AMD RZ616 (`14c3:0616`, subsystem `14c3:061c`, chip rev `0x10`).
MT7921 support uses the same code paths but is untested. Reports welcome.

## What works

- Firmware download and boot (ROM patch + WM RAM code)
- Firmware-offloaded scanning, networks listed in Haiku's network menu
- Joining open and **WPA2-PSK** networks (encryption in software by net80211)
- DHCP, normal IP traffic
- **802.11n** at 20 MHz, two streams (up to 144 Mbit/s), with receive
  block-ack sessions and WME/QoS. Turn it off with `ht off` (see below).
- 2.4 GHz channels 1–11 and 5 GHz 36–165, including the DFS channels
  52–144 (listen-only until an access point is heard)
- MSI interrupts; DMA is paused and bus mastering off while the interface is down
- DMA is stopped as soon as Haiku starts shutting down or restarting

## Current limitations

- **No 40/80 MHz, 802.11ac or 802.11ax yet**, and no transmit aggregation,
  so uploads are slower than downloads.
- **No country/regulatory data yet**; the channel list is a fixed, US-safe one.
- **Warm reboots:** Haiku has no driver shutdown hook, and a warm reboot does
  not reset this chip. The driver now watches for Haiku's shutdown and stops
  DMA in time, and also stops leftover DMA when it loads. See
  [docs/TESTING.md](docs/TESTING.md).
- No power saving, no hardware crypto, no suspend/resume, no 6 GHz.

## Building

You need the Haiku source tree. Building the driver alone takes a few minutes
on Haiku itself, with no cross-compiler.

```sh
# Haiku source, once
cd ~
git clone --depth 1 https://review.haiku-os.org/haiku
cd haiku && mkdir generated.x86_64 && cd generated.x86_64 && ../configure

# This driver, inside the tree
cd ~/haiku/src/add-ons/kernel/drivers/network/wlan
git clone https://github.com/Techniix/mediatekwifi7921_HaikuOS mediatekwifi7921
echo 'SubInclude HAIKU_TOP src add-ons kernel drivers network wlan mediatekwifi7921 ;' >> Jamfile

# Build
cd ~/haiku/generated.x86_64
jam -q mediatekwifi7921
```

The helper script wraps the usual steps:

```sh
cd ~/haiku/src/add-ons/kernel/drivers/network/wlan/mediatekwifi7921
tools/mt7921dev.sh build      # jam -q mediatekwifi7921
tools/mt7921dev.sh install    # copy the driver to non-packaged
tools/mt7921dev.sh firmware   # check the firmware files are in place
tools/mt7921dev.sh enable     # load at next boot
tools/mt7921dev.sh disable    # don't load at next boot
tools/mt7921dev.sh log        # driver lines from the syslog
```

## Firmware

The firmware is not part of this repository. Get it from
[linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/mediatek)
(use each file's *plain* link) and put it in:

```
/boot/system/non-packaged/data/firmware/mediatekwifi7921/
```

## Installing and enabling

```sh
tools/mt7921dev.sh install
tools/mt7921dev.sh enable
shutdown -r
```

Then pick your network from the Deskbar network menu.

**If something goes wrong:** run `tools/mt7921dev.sh disable`, then **shut
down and cut power for ~30 seconds** (a warm reboot may not reset the chip).
If you can't reach a Terminal, hold **Shift** at boot and choose
**Disable user add-ons**.

## Bring-up stages

Attach can stop after any stage, which is handy when testing a new build or
new hardware. Put `stage N` in
`~/config/settings/kernel/drivers/mediatekwifi7921`:

| Stage | Does |
|---|---|
| 1 | Chip ID, reset, firmware files present (then detaches) |
| 2 | + DMA rings up (then detaches) |
| 3 | + firmware boot, MAC address (then detaches) |
| 4 | + interrupts, WiFi interface registered (radio hooks stubbed) |
| 5 | + MAC/radio setup, scanning |
| 6 | + transmit, joining networks (**default**) |

The same file takes `ht off` to turn 802.11n off and run at legacy rates
(up to 54 Mbit/s), as version 0.1.0 did. `tools/mt7921dev.sh ht off` sets it.

## Layout

```
Jamfile, glue.c          Haiku build file and FreeBSD-compat glue
dev/mt7921/
  if_mt7921.c            PCI attach/detach, register access, interrupts
  mt7921_dma.c           WFDMA rings (RX, TX, firmware download)
  mt7921_mcu.c           Firmware download and MCU commands
  mt7921_net.c           net80211 glue: RX/TX, scanning, joining
  mt7921_reg.h           Registers (from mt76)
  if_mt7921var.h         Softc and shared declarations
docs/PORTING.md          Design notes, mt76 mapping, milestones
docs/TESTING.md          Test procedure and recovery
tools/mt7921dev.sh       Build/install/enable helper
```

## License

The driver is MIT-licensed (see [LICENSE](LICENSE)). The parts derived from
mt76 keep their original copyright notices and the BSD-3-Clause-Clear
license (see [LICENSE.mt76](LICENSE.mt76)). Both are compatible with Haiku.

## Credits

- MediaTek and the mt76 authors (Felix Fietkau, Lorenzo Bianconi and many
  others) for the Linux driver this is ported from
- The FreeBSD wireless developers for `net80211`, and Haiku's developers for
  the FreeBSD compatibility layer
