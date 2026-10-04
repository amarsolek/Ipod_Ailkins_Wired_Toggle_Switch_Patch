# Rebuilding the firmware and plugins

Current target commit is in `BASED_ON_COMMIT.txt`
(`bef221afd70454d948b21e9e6bc2dfd43002b273`, 2026-10-02). Verified on
Ubuntu 24.04 in October 2026.

## Why the commit matters

Rockbox refuses to load a plugin whose `PLUGIN_API_VERSION` differs from the
running firmware, reporting **"Incompatible version"**. The value is bumped by
upstream without warning:

| Commit | Date | `PLUGIN_API_VERSION` |
|---|---|---|
| `943b73851e` | 2026-08 | 284 |
| `bef221afd7` | 2026-10 | 285 |

Installing a newer firmware without rebuilding the plugins is what broke the
device on 2026-10-03: the official build overwrote `rockbox.ipod` and every
stock `.rock`, but had no `Adam's_Playlists.rock` to overwrite, so the API 284
plugin was left behind under an API 285 firmware.

**Firmware and plugins must always be built from the same commit and installed
together.**

## 1. Cross-toolchain

Rockbox expects an `arm-elf-eabi-` prefix; the distro package ships
`arm-none-eabi-`, so symlink it:

```sh
sudo apt-get install -y gcc-arm-none-eabi binutils-arm-none-eabi \
                        libnewlib-arm-none-eabi make perl zip
for t in gcc ld objcopy objdump ar as ranlib nm strip size cpp \
         gcc-ar gcc-ranlib gcc-nm; do
  sudo ln -sf "$(which arm-none-eabi-$t)" /usr/local/bin/arm-elf-eabi-$t
done
```

configure warns that the compiler is not its recommended 9.5.0. The warning is
safe: 10.3.1, 13.2.1 and 13.3.0 all produce an identical plugin header.

## 2. Source tree

GitHub's API is not always reachable and does not accept abbreviated SHAs for
archive downloads. Use the project's own gerrit mirror, which does serve
`git fetch`:

```sh
mkdir rockbox && cd rockbox && git init -q
git remote add origin https://gerrit.rockbox.org/r/rockbox
git fetch -q --depth 200 origin master
git checkout -q "$(cat ../BASED_ON_COMMIT.txt)"
```

A depth of 200 is enough to reach a commit a few days behind master; increase
it for an older target.

## 3. Apply the customizations

```sh
git apply ../port-to-bef221afd7.patch
cp ../playlist_art.c     apps/plugins/playlist_art.c
cp ../clearcache.fixed.c apps/plugins/clearcache.c
```

The patch covers all eight tracked files. Two of them are easy to miss:

- **`apps/root_menu.h`** — adds `GO_TO_ADAMS_PLAYLISTS` to the root-item enum.
  Without it `root_menu.c` fails with *"'GO_TO_ADAMS_PLAYLISTS' undeclared"*.
- **`apps/menus/plugin_menu.c`** — note the `menus/` directory. There is no
  `apps/plugin_menu.c`.

## 4. Keep the local mikey driver; do NOT swap in upstream's

Upstream merged a mikey remote driver, and it is tempting to drop the local
`mikey-6g.c` in favour of it because it is newer and larger (13,168 bytes vs
9,576). **Do not.** It was validated on other units and decodes this one
differently. `port-to-bef221afd7.patch` keeps the local driver; `mikey-6g.c`
in this folder is the authoritative copy.

The two differ in ways that matter on this iPod:

| | local driver | upstream |
|---|---|---|
| centre mask | `MIKEY_BTN_CENTER 0x01` | `0x05` (bits 0 and 2) |
| volume codes | `BUTTON_RC_VOL_UP` / `_DOWN` | `BUTTON_MULTIMEDIA_VOLUME_*` |
| phantom-load suppression | none | arm-window suppression |

Bit 2 of reg4 was measured on this device as a unit-dependent base level, not
the centre button. Including it, as upstream does, makes some remotes' volume
press register as a centre press - the player pauses instead of changing
volume.

The volume codes matter for a second reason. `BUTTON_MULTIMEDIA_VOLUME_UP` is
`(BUTTON_MULTIMEDIA|0x40)`, and ipod6g's `POWEROFF_BUTTON` is `BUTTON_PLAY ==
0x40`, so the generic `btn & POWEROFF_BUTTON` test in
`firmware/drivers/button.c` matches every volume-up report and shuts the
device down past `POWEROFF_COUNT` repeats. `BUTTON_RC_VOL_UP` (0x00010000)
carries no low-byte bits and cannot. The patch also fixes the button.c test
itself (section 9), so either code path is safe, but the local driver avoids
the collision by construction.

Upstream did not take `HAVE_VOLUME_IN_LIST` either. Without it the remote's
volume works only in the WPS and quickscreen; the patch adds the define and
both `keymap-ipod.c` blocks so it works in menus and the file browser too.

One upstream symbol is required: `debug-s5l8702.c` calls `mikey_present()`.
The local driver provides it as a latch set once the chip ACKs the bus.

### Hardware note: verify the remote before blaming the firmware

Apple's 3-button remote (EarPods with 3.5mm plug, MWU53AM/A) is **confirmed
working** on this iPod: Apple's own compatibility list names the 120GB iPod
classic as supporting the volume buttons, and it was verified on this device
on 2026-10-04.

Third-party "MFi certified" 3.5mm earbuds are not Apple-protocol; the MFi
claim is unverifiable on an analog headset, which has no authentication chip.
An AILKIN pair decoded correctly for one evening and then failed - volume-up
reading as centre, volume-down sticking - while Apple headphones on the same
firmware worked fine.

**Before changing any firmware to chase a remote bug, test with Apple
headphones.** Several hours were lost on 2026-10-04 rebuilding firmware
against a failing third-party remote. Identical binaries behaving differently
between sessions is a hardware symptom, not a software one.

## 5. Configure and build

```sh
mkdir build && cd build
printf '29\nN\n' | ../tools/configure     # 29 = ipod6g, N = Normal
make -j"$(nproc)"
```

Outputs: `rockbox.ipod`, `apps/plugins/playlist_art.rock`,
`apps/plugins/clearcache.rock`.

## 6. Verify before installing

```sh
python3 - <<'PY'
import struct, os
for p in ['apps/plugins/playlist_art.rock','apps/plugins/clearcache.rock']:
    b = open(p,'rb').read(16)
    target, api = struct.unpack('<HH', b[4:8])
    load, = struct.unpack('<I', b[8:12])
    print(f"{os.path.basename(p):<22} magic={b[0:4].decode()} "
          f"target={target} api={api} load=0x{load:08X}")
PY
```

Expected for this device:

| Field | Value |
|---|---|
| magic | `KcoR` (RocK on disk) |
| target_id | 71 (ipod6g) |
| api_version | 285 at `bef221afd7` — must equal the firmware's |
| load address | 0x0BDFC000 |

`api_version` comes from `PLUGIN_API_VERSION` in `apps/plugin.h`, `target_id`
from `MODEL_NUMBER` in `firmware/export/config/ipod6g.h`.

Image-viewer overlays (`bmp.ovl`, `gif.ovl`, `jpeg.ovl`, `jpegp.ovl`,
`png.ovl`, `ppm.ovl`) report `api=1`. That is their own versioning scheme and is
not a mismatch.

## 7. Install

Back up `/.rockbox` first, then copy all four together:

| Build output | Destination on the iPod |
|---|---|
| `rockbox.ipod` | `/.rockbox/rockbox.ipod` |
| `rockbox-info.txt` | `/.rockbox/rockbox-info.txt` |
| `apps/plugins/playlist_art.rock` | `/.rockbox/rocks/apps/Adam's_Playlists.rock` |
| `apps/plugins/clearcache.rock` | `/.rockbox/rocks/apps/Clear_Cache.rock` |

The two plugins are **renamed on install** — those exact names are what the root
menu entry and the Plugins menu look for.

Nothing here needs DFU. The bootloader loads `/.rockbox/rockbox.ipod` from the
FAT32 partition, so updating the firmware is an ordinary file copy. DFU is only
needed to install or remove the bootloader itself.

`/.rockbox/Adams_Playlists/` holds the plugin's cache and your settings
(`custom.ord`, `plsort.cfg`, `sort.cfg`). Leave it alone across a rebuild —
`custom.ord` is your hand-arranged playlist order and is not regenerable.

## 8. After any Rockbox Utility reinstall

Rockbox Utility replaces everything under `/.rockbox`. Re-copy the firmware and
both plugins afterwards, or you land back on "Incompatible version".

## 9. ipod6g gotcha: never report volume as a multimedia key

`port-to-bef221afd7.patch` changes the mikey driver to emit
`BUTTON_RC_VOL_UP` / `BUTTON_RC_VOL_DOWN` instead of upstream's
`BUTTON_MULTIMEDIA_VOLUME_UP` / `_DOWN`. **Do not revert this.**

On ipod6g the bits collide with the power button:

```
BUTTON_MULTIMEDIA_VOLUME_UP = BUTTON_MULTIMEDIA|0x40 = 0x10000040
POWEROFF_BUTTON             = BUTTON_PLAY           = 0x00000040
                                          AND       = 0x40
```

`firmware/drivers/button.c` decides to shut down with
`btn & POWEROFF_BUTTON`, so that test is true on every volume-up report.
Once `repeat_count` passes `POWEROFF_COUNT` (40) it calls `sys_poweroff()`
and the device powers off mid-press. ipod6g also defines
`HAVE_POWEROFF_WHILE_CHARGING`, so the `!charger_inserted()` guard is
compiled out and it happens on the charger too. Volume-down is unaffected
(`0x10000080 & 0x40 == 0`); only volume-up triggers it.

Observed 2026-10-03 on build `bef221afd7M-261004`: holding remote volume-up
ramped the volume to maximum and then shut the iPod down.

`BUTTON_RC_VOL_UP` (0x00010000) and `BUTTON_RC_VOL_DOWN` (0x00008000) carry
no low-byte bits. They are also what `keymap-ipod.c` already maps for WPS and
quickscreen, and what the `HAVE_VOLUME_IN_LIST` entries in this patch map for
the list and tree contexts — so the keymap and the driver finally agree.

The trade-off: volume no longer goes through the global multimedia handler in
`apps/misc.c`, so it is keymap-driven. That is why the `HAVE_VOLUME_IN_LIST`
entries matter; without them volume would work only in the WPS and
quickscreen.

This is a live upstream bug, not specific to this tree: 57 targets define a
`POWEROFF_BUTTON` with low-byte bits, and any of them that starts emitting
multimedia volume codes will hit the same wall. ipod6g is simply the first
target where something emits them, because the mikey driver is new.

## 10. Battery curve: keep 0% well clear of the shutoff

`percent_to_volt_discharge[0]` is 3500 mV, matching upstream, and
`battery_level_shutoff` is 3300 mV. An earlier revision of this tree used
3320 mV for 0% — only 20 mV of headroom — so a load transient could cross
the hard cutoff while the gauge still read above zero. `battery_level_disksafe`
is also 3500, so 0% and the no-disk-writes threshold now coincide, which is
the intended relationship.

## 11. The inline remote: measured register map

Decoding was settled on 2026-10-04 by capture from the device, after several
builds guessing from upstream comments written against other people's units.
The debug screen that produced it is in this patch (hardware info, page 2);
it latches in the polling thread, because the debug screen's own loop exits
on button release and reg5 events only persist ~100ms.

Captured from a third-party (AILKIN) remote on this iPod:

| state | r4 | r5 |
|---|---|---|
| idle | `0x20` | `0x00` |
| volume up | `0x24` | `0x04` |
| volume down | `0x24` | `0x01` |
| centre | `0x25` | `0x00` |

So `r4` bit5 is a constant base level, **bit2 means a button is down**, and
**bit0 means centre**. Over 32 samples no release event (`0x08` / `0x02`)
was ever observed.

Three conclusions, each of which cost a build to learn:

1. **Both Apple and third-party remotes use bit0 for centre.** An earlier
   version latched `apple_style = true` on first sight of bit0 and switched
   decoders, so the first centre press permanently disabled volume for the
   rest of the session. There is nothing to auto-detect. Do not reintroduce
   remote-type detection.
2. **A centre mask is not the lever.** `0x05` includes bit2 and so fires on
   every button (volume-up pauses the player); `0x01` alone never matches on
   a remote whose centre is `0x25`... except it does, which is why `0x01` is
   correct — but only once the decoder stops treating bit2 as centre. The
   earlier symptom-chasing between `0x05` and `0x01` was a wrong model, not
   a wrong constant.
3. **A volume hold must be able to end on a level.** Apple remotes send a
   release event; this one sends none, so waiting for one let volume run to
   the rail. The hold now clears on a release event **or** `r4` bit2 going
   clear, with a 2s cap as a backstop. A stuck `mikey_btn` ORs into every
   physical button read and makes the click wheel appear dead, so this also
   protects the main UI.

## 12. Responsiveness: leave the timing alone

Measured latency from button press to volume step is about 30ms - a 20ms
driver poll plus the 10ms button tick. There is nothing worth reclaiming.

What a hold "feels" like is Rockbox's global repeat timing in
`firmware/drivers/button.c`: `REPEAT_START` 300ms, `REPEAT_INTERVAL_START`
160ms, decaying to `REPEAT_INTERVAL_FINISH` 50ms. Those apply to every
button including the click wheel; changing them to tune the remote retunes
the whole player.

Do not shorten the driver's `sleep(HZ/50)` poll. Mikey shares **I2C bus 0**
with `cscodec-6g` (the audio codec) and `pmu-6g`. Doubling the poll rate
doubles this driver's traffic on the bus carrying audio, to buy back at most
10ms nobody can perceive.
