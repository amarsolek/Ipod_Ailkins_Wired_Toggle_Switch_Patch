# Playlist Art v2 for iPod Classic (Rockbox plugin)

Browse the playlists you create in **iTunes** on your **iPod Classic**, with
**album cover thumbnails** next to every track — the feature neither the Apple
firmware nor stock Rockbox has.

**What you get**

- Your iTunes playlists appear automatically (parsed from the iPod's iTunesDB —
  keep syncing with iTunes exactly as you do today)
- **v2:** artwork is read straight from the iPod's own ArtworkDB (the
  pre-rendered art iTunes writes for the device), so store-downloaded
  artwork, PNGs, and progressive JPEGs all display — embedded JPEG and
  cover.jpg files remain as fallbacks, and several tracks per album are
  tried before giving up
- Click into a playlist: every track shows its album artwork, title, artist,
  and star rating
- Status bar with clock, battery %, play/pause icon, and hold-switch indicator
- Click wheel scrolling, scrolling (marquee) text for long titles
- **Recently Added** (v2.2) — an automatic playlist of your ~200 newest
  tracks, newest first, always pinned at the top
- Press SELECT on a track to play it — you land on Rockbox's normal
  now-playing screen, where your chosen theme provides big album art, progress
  bar, elapsed/remaining time, and click-wheel volume
- Your playlists are also exported as normal `.m3u8` files in `/Playlists`,
  usable from Rockbox's own menus too

**What's in this folder**

| File | What it is |
|---|---|
| `rockbox-ipodclassic-playlist-art.zip` | Complete custom Rockbox build for iPod Classic (6th/7th gen) with the plugin included |
| `playlist_art.c` | Plugin source code |
| `playlist_art.patch` | Patch against the Rockbox source tree (for rebuilding yourself) |
| `BASED_ON_COMMIT.txt` | The exact Rockbox git commit this build is based on |

---

## IMPORTANT — read first

- This is an **experimental, unofficial build**. It compiled cleanly, but it
  has **not yet been tested on real hardware**. Try it when you have time to
  fiddle, not right before a trip.
- Nothing it does is destructive: your music, your iTunes sync, and the Apple
  firmware all stay untouched. Worst case you delete two folders and you're
  back to stock (see Uninstall).
- Your iPod must be **Windows-formatted (FAT32)**. iPods set up on a Mac
  (HFS+) must be restored in iTunes on Windows first.
- The plugin only works in **this** build. It will NOT load on an official
  Rockbox install (plugin versions must match the build exactly).

---

## Installation

### Step 1 — Enable disk use

1. Connect the iPod to your PC, open iTunes.
2. Select the iPod, tick **"Enable disk use"** (Summary page), Apply.
   The iPod now also appears as a drive letter in File Explorer.

### Step 2 — Install Rockbox (official installer does the hard part)

1. Download **Rockbox Utility** from https://www.rockbox.org/download/
2. Run it with the iPod connected. Let it autodetect the iPod.
3. Choose **Complete Installation** — this installs the bootloader and an
   official Rockbox build. Follow its prompts (the iPod Classic bootloader
   install involves putting the iPod into DFU mode; the Utility walks you
   through it — read each prompt carefully).
4. Also install the **font pack** when offered (the Utility usually includes
   it in Complete Installation).
5. When it finishes, the iPod boots into Rockbox. Verify that works first.

### Step 3 — Copy this custom build over it

1. With the iPod connected, open its drive letter in File Explorer.
2. In Explorer: View -> Show -> **Hidden items** (the `.rockbox` folder is
   hidden).
3. Open `rockbox-ipodclassic-playlist-art.zip` from this folder. Drag the
   `.rockbox` folder inside it onto the iPod's drive, choosing
   **Replace the files in the destination** when asked.
4. Safely eject the iPod, then restart it (hold SELECT+MENU until it reboots).

That's it. Your custom build (with the plugin) is now the installed Rockbox.

---

## Running the plugin

1. On the iPod, go to: **Main menu -> Plugins -> Applications ->
   playlist_art**
2. **First run:** it finds the iTunesDB, reads your library and playlists,
   and builds the album-art thumbnail cache. A progress bar shows each phase.
   With a big library this can take several minutes (one-time cost — every
   album cover gets decoded once). Keep the iPod on its charger for a huge
   library.
3. You'll see your playlists. Controls:

| Control | Action |
|---|---|
| Click wheel | Scroll |
| SELECT | Open playlist / play track |
| MENU (in a playlist) | Back to playlist list |
| MENU (on playlist list) | Options menu (update, rebuild, help, exit) |
| LEFT | Back / exit |

4. Playing a track jumps to Rockbox's now-playing screen (WPS). Volume,
   pause, skip, progress bar, big album art etc. all work there — pick any
   theme you like (Settings -> Theme Settings, or install themes with
   Rockbox Utility from https://themes.rockbox.org/index.php?target=ipod6g ).

**After every iTunes sync:** just open the plugin — it notices the iTunesDB
changed and re-syncs automatically. New albums get their art cached; already
cached albums are skipped, so updates are much faster than the first run.

**Options menu** (press MENU on the playlist list):

- *Sort playlists* — Newest first (default), Alphabetical, iTunes order, or
  Custom (my order). Remembered across restarts.
- **Custom ordering (v2.3):** hold SELECT on any playlist to pick it up,
  scroll to move it, press SELECT to drop it. The order is saved and the
  sort mode switches to Custom automatically. Your arrangement survives
  iTunes re-syncs (playlists are matched by name).
- *Update from iTunes* — re-read iTunesDB now
- *Full rebuild (incl. artwork)* — wipe assumptions, re-decode all artwork
- *Exit plugin*

---

## Uninstalling

### Just go back to official Rockbox (keep Rockbox, lose only my build)

1. Run Rockbox Utility -> Installation -> install the official build.
   That overwrites this custom `.rockbox` with the official one.
2. Optionally delete the plugin's data: the `/Playlists` folder and the
   hidden `/.rockbox/playlist_art` folder on the iPod.

### Remove Rockbox completely (back to Apple firmware only)

1. Run **Rockbox Utility** -> **Uninstallation** tab -> uninstall the
   bootloader (it walks you through DFU mode again), or follow the
   uninstall section of the Rockbox manual for iPod Classic.
2. Delete the `.rockbox` and `/Playlists` folders from the iPod's drive.
3. Done — the iPod boots the Apple firmware as before.

### Nuclear option (if anything ever seems broken)

Connect to iTunes and **Restore** the iPod. This wipes the disk back to
factory state (you'll re-sync your music afterwards). This always works —
Rockbox cannot brick an iPod Classic in a way Restore doesn't fix.

### Meanwhile, you always have dual boot

The Apple firmware is still installed. To boot it instead of Rockbox:
turn the iPod on and immediately **hold MENU** (or flick the Hold switch on
right after powering on). To get back to Rockbox, reboot (SELECT+MENU) with
no buttons held.

---

## Troubleshooting

- **"No iTunesDB found"** — the iPod hasn't been synced with iTunes, or disk
  use isn't enabled. Sync once in iTunes, then run the plugin again.
- **"This iTunesDB is compressed (iTunesCDB)"** — some late iPod firmware
  versions compress the database; this v1 doesn't decompress it yet. Tell me
  and I'll add zlib support in v2.
- **A track plays the wrong song / playlist looks wrong** — iTunesDB field
  offsets vary slightly between iTunes versions; report it, this is fixable.
- **Some albums show "?" instead of art** — with v2 this should be rare:
  it means the album has no art in iTunes at all, or its tracks were synced
  before iTunes had the art (re-sync, then run *Update from iTunes* in the
  plugin menu). A `cover.jpg` in the album folder also works as a fallback.
- **Upgrading from v1:** copy the new `.rockbox` folder over the old one
  (Step 3), then open the plugin and run *Update from iTunes* — albums that
  showed "?" get retried against the ArtworkDB automatically. Run *Full
  rebuild (incl. artwork)* instead if you want every thumbnail regenerated
  from the ArtworkDB source.
- **Ratings blank** — songs rated on the iPod itself sync back differently;
  rate in iTunes and re-sync.
- **Plugin says "Incompatible version"** — you're running it on an official
  Rockbox build; reinstall the custom build (Step 3).
- **Rest of Rockbox shows no music** — that's normal until you initialize
  Rockbox's own database: Main menu -> Database -> Initialize Now. (The
  plugin doesn't need this, but it lets the rest of Rockbox see your
  iTunes-synced files too.)
- **Connecting USB** — exit the plugin first, then plug in.
- **Songs missing from playlists (v2.1)** — limits are now 60,000 library
  tracks and 10,000 tracks per playlist. After every sync the plugin shows
  a summary ("Synced N tracks, P playlists") and, if anything was skipped,
  a second message saying how many and why. If you see "over cap" there,
  tell me and I'll raise the limits further.

## Rebuilding from source (optional)

Should you ever want to rebuild or modify it: clone the Rockbox source
(commit in `BASED_ON_COMMIT.txt`), apply `playlist_art.patch`, install the
Rockbox arm-elf-eabi toolchain (`tools/rockboxdev.sh`), then:
`mkdir build && cd build && ../tools/configure --target=ipod6g --type=n &&
make -j8 zip`. The result is a `rockbox.zip` like the one here.
