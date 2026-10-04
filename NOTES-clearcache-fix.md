# clearcache plugin - the FAT32 corruption bug (Sept 2026)

## Symptom

After running **Clear Cache -> "Clear all cache (art + playlists)"** on the
iPod, iTunes refused to sync:

> Attempting to copy to the disk "ADAMM'S IPO" failed.
> The file or directory is corrupted and unreadable.

Windows had set the volume dirty bit. `I:\Playlists` and
`I:\.rockbox\Adams_Playlists` both threw the same error just being listed.

## Cause

`clearcache.c` deleted files **inside** the `rb->readdir()` loop, while the
directory was still open:

```c
DIR *d = rb->opendir(dir);
while ((e = rb->readdir(d))) {
    rb->snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    rb->remove(path);          /* <-- mutating the directory mid-walk */
}
rb->closedir(d);
```

Rockbox's directory iterator holds a cursor into the directory's cluster
chain plus cached directory sectors. `rb->remove()` rewrites those same
sectors (marking entries 0xE5, freeing cluster chains), so the open handle's
cached sector and entry index go stale. With long filenames - one file spans
several consecutive 32-byte entries - the walk lands inside orphaned LFN
fragments, and stale cached sectors get written back over updated ones.

With 4,289 files in `thumbs/` this produced exactly what chkdsk reported:
garbage entry names, "first allocation unit is not valid", "Folder has
non-zero file size", invalid timestamps. It also left the FAT free-cluster
accounting wrong, so iTunes' *next* writes landed on bad chains - 857 broken
files and the failed sync.

## Fix

`clearcache.fixed.c`: collect a batch of names, **close the directory**, then
delete; repeat until a pass finds nothing. Bounded memory, no open handle
during any removal, plus a guard so undeletable files cannot spin forever:

```c
if (found > 0 && removed == 0) break;   /* cannot make progress */
```

## Verification

The two functions were extracted verbatim and compiled natively against real
directories, with `remove()` instrumented to count deletions issued while a
`DIR` handle was open:

```
ORIGINAL   removed=4289  opendir_passes=1   DELETE_WHILE_OPEN_VIOLATIONS=4289
FIXED      removed=4289  opendir_passes=91  DELETE_WHILE_OPEN_VIOLATIONS=0
```

Built for ipod6g against upstream commit in BASED_ON_COMMIT.txt. Header of
the resulting .rock matches the plugin already working on the device:
magic `RocK`, target id 71, API version 284, load address 0x0BDFC000.

On-device run of "Clear album artwork": 4,270 of 4,289 removed, volume stayed
clean, and the post-run scan showed **zero** invalid timestamps, **zero**
"folder has non-zero file size", **zero** mangled names. The 19 it could not
remove were already broken ("first allocation unit is not valid") - written
onto the corrupt filesystem before the first repair - and the guard made it
stop cleanly rather than loop.

## Files

| File | What it is |
|---|---|
| `clearcache.c` | original, buggy - kept for reference |
| `clearcache.orig.c` | identical backup of the above |
| `clearcache.fixed.c` | corrected source (this is the one to build) |
| `Clear_Cache.FIXED.rock` | built from clearcache.fixed.c, installed on the iPod |
| `Clear_Cache.BACKUP.rock` | the buggy binary that was on the iPod |

## Safer alternative

`C:\Users\adamm\Documents\albumart-guard\Clear-iPodCache.ps1` (desktop
shortcut "Clear iPod Cache") does the same three operations from Windows,
where the OS filesystem driver handles the deletion. Preferred for bulk
clears; the plugin is the on-the-go option.
