# iPod Classic 6G/7G — wired inline-remote patch for Rockbox

Working volume-up, volume-down and play/pause from a **3.5 mm wired inline
remote** on an iPod Classic 6th/6.5th/7th gen running Rockbox, including
cheap third-party remotes (developed and verified against **Ailkin** wired
earbuds, and also tested with genuine Apple EarPods).

Based on Rockbox commit `bef221afd70454d948b21e9e6bc2dfd43002b273`
(`PLUGIN_API_VERSION` 285). See `BASED_ON_COMMIT.txt`.

---

## Install

A prebuilt, verified binary is attached to the
[latest release](https://github.com/amarsolek/Ipod_Ailkins_Wired_Toggle_Switch_Patch/releases/latest)
as `rockbox-ipod6g-remote-fix-v1.0.zip`. Unzip it to the root of the iPod so
the `.rockbox` folder lands at the top level, replacing what is there. You
still need the Rockbox bootloader installed (use Rockbox Utility for that),
and Rockbox Utility is also the easiest way to add fonts, which are left out
of the zip to keep it small.

Verify before installing:

    .rockbox/rockbox.ipod            1,150,140 bytes  md5 ee39390205fefad0ff18f73cab0fe482
    .rockbox/rocks/apps/Adam's_Playlists.rock  24,560 bytes
    .rockbox/rocks/apps/Clear_Cache.rock        2,696 bytes

Plugins must match the firmware's `PLUGIN_API_VERSION` (285 here) exactly, or
Rockbox reports "Incompatible version" and refuses to load them. Do not mix
plugins from one build with firmware from another.

## What this fixes

**1. One decoder for both remote styles.** Earlier attempts tried to
auto-detect "Apple style" vs "third-party style" remotes. That detection was
the bug: **both** styles signal the centre button with bit 0 of Mikey
register 4, so the first centre press latched the device into the Apple code
path and killed volume control until reboot. The driver in `mikey-6g.c` now
runs a single decoder built from registers measured on real hardware:

| state | reg 4 | reg 5 |
|---|---|---|
| idle | `0x20` | — |
| volume button down | `0x24` | `0x04` up / `0x01` down |
| centre button down | `0x25` | — |

Bit 5 (`0x20`) is a constant base level, bit 2 (`0x04`) means *some* button
is down, bit 0 (`0x01`) means the centre button specifically. Register 5
carries edge events and holds them for roughly 100 ms.

**2. Volume-up no longer powers the iPod off.** This one is upstream, not
local. `firmware/drivers/button.c` tests `btn & POWEROFF_BUTTON`, but
multimedia key codes are a separate namespace from physical button bits:

```
BUTTON_MULTIMEDIA_VOLUME_UP  = 0x10000040
POWEROFF_BUTTON (ipod6g)     = BUTTON_PLAY = 0x40
```

The AND is non-zero, so every volume-up report from a remote looked like a
poweroff request. **57 targets** define a `POWEROFF_BUTTON` with bits in the
low byte and are affected the same way. The fix is a guard excluding
multimedia codes before the poweroff test.

**3. Battery 0% floor raised.** `powermgmt-6g.c` had the 0% point at
3320 mV, only 20 mV above `battery_level_shutoff` (3300 mV) — enough that a
normal load transient could cross it. Raised to 3500 mV.

## Contents

| File | What it is |
|---|---|
| `port-to-bef221afd7.patch` | **The patch.** All 12 changed files against the base commit above |
| `mikey-6g.c` | The inline-remote I²C driver (the authoritative copy — see BUILDING.md §4) |
| `mikey-remote-driver.patch` | Earlier standalone driver patch, kept for reference |
| `powermgmt-6g.c` | Battery curve with the raised 0% floor |
| `BUILDING.md` | How to rebuild: toolchain, configure, the gotchas worth knowing |
| `BASED_ON_COMMIT.txt` | Exact upstream commit this is based on |
| `playlist_art.c`, `playlist_art.patch`, `root_menu.*`, `plugin_menu.c` | The Playlist Art plugin this tree also carries — see `PLAYLIST_ART.md` |
| `clearcache*.c`, `NOTES-clearcache-fix.md` | Clear Cache plugin and its fix |

Read **`BUILDING.md`** before rebuilding. In particular §4 (do not replace
the local Mikey driver with upstream's), §9 (the multimedia/POWEROFF
collision), §11 (the measured register map) and §12 (leave the button timing
alone — it shares I²C bus 0 with the audio codec).

## Hardware notes

The "Mikey" chip sits on I²C bus 0 at address `0x72` and decodes the
headphone jack's mic and inline remote. It shares that bus with
`cscodec-6g` (audio codec) and `pmu-6g`, which is why the polling interval
is left as upstream set it.

## Licence

Rockbox is GPLv2+. The changes here are distributed as a patch against the
named upstream commit and carry the same licence.
