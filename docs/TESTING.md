# Testing a build

## The test loop

```sh
cd ~/haiku/src/add-ons/kernel/drivers/network/wlan/mediatekwifi7921
tools/mt7921dev.sh build && tools/mt7921dev.sh install
tools/mt7921dev.sh stage 6        # or a lower stage, see README
tools/mt7921dev.sh enable
shutdown -r
```

After the reboot:

```sh
tools/mt7921dev.sh log            # last driver lines from the syslog
tools/mt7921dev.sh disable        # when you are done testing
```

Haiku rotates `/var/log/syslog` to `/var/log/syslog.1` when it gets big. The
`log` command reads both, oldest first, so the newest lines are at the end.

## Ending a session: shut down, don't restart

Haiku gives drivers no shutdown hook, and a warm reboot on the test machine
does **not** reset the MT7922: register state (including enabled DMA rings)
was observed to survive a restart. A chip with live DMA rings can write into
memory that the next boot has already reused.

The driver defends against this:

- At load it logs `PCI command at load: 0x.... (bus master ON/off)`, which
  shows whether the BIOS left bus mastering on across the restart.
- Before enabling bus mastering it stops any leftover DMA and logs
  `stopped DMA left running by a previous run` if it found some.
- While the interface is down, DMA is paused and bus mastering is off.

Until this is solved properly, **end test sessions with a full shutdown** and
cut power for ~30 seconds.

## Recovering from a bad build

1. `tools/mt7921dev.sh disable` (removes the `dev/net` link, keeps the binary)
2. Shut down, cut power ~30 s, boot.
3. No Terminal? Hold **Shift** at boot → **Disable user add-ons**.

## What a good stage-6 log looks like

```
bring-up stage 6 of 6
PCI command at load: 0x.... (bus master off)
ASIC revision: 79220010 (chip 0x7922 rev 0x10)
ROM patch loaded
WM firmware: version ____000000-2026..., 6 regions
firmware running
MAC address xx:xx:xx:xx:xx:xx
MAC and radio set up
registered with net80211 (31 channels)
interrupts enabled (MSI)
WiFi interface created
DMA resumed
scan started: 20 channels, active, 0 SSIDs
rx frame: fc 8000 ...            (beacons)
scan done (N frames received so far)
net80211 state SCAN -> AUTH
joining xx:xx:... on channel N: channel granted
tx: fc b000 ...  /  tx done: token 0 status 0     (authentication)
tx: fc 0000 ...  /  tx done: token 1 status 0     (association request)
net80211 state ASSOC -> RUN
associated with xx:xx:... (AID n)
tx: fc 0801 ...                                   (WPA handshake / data)
tx: fc 0841 ...                                   (encrypted data)
```

A `tx done` status other than 0 means the firmware failed to send the frame.

## Reporting a problem

Please include: the card's PCI ID (`listdev`), the stage, the output of
`tools/mt7921dev.sh log`, and whether wired networking and Deskbar still
worked.
