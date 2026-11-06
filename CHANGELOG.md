# Changelog

## 0.1.0 — 2026-11-05

First working version, tested on an AMD RZ616 (MT7922).

- Milestone 1: PCI attach, BAR0 remap, driver/firmware ownership handshake,
  chip ID, WF subsystem reset, firmware file loading.
- Milestone 2: WFDMA rings (polled), safe teardown order (DMA off, bus
  mastering off, core reset, then free memory), scrub of leftover DMA state
  at load.
- Milestone 3: firmware download (ROM patch + WM RAM code), NIC capability,
  MAC address.
- Milestone 4: MSI interrupts, net80211 registration, WiFi interface; DMA
  paused while the interface is down.
- Milestone 5: MAC/radio setup, RX descriptor parsing, firmware-offloaded
  scanning.
- Milestones 6–7: TX path (TXD + TXP, TX-free tokens), joining (STA_REC,
  ROC, BSS_INFO), open and WPA2-PSK networks with software crypto.
