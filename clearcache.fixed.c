/***************************************************************************
 * clearcache - clear the Adam's Playlists cache (artwork / playlist data)
 *
 * FIXED VERSION.
 *
 * The original deleted files inside the rb->readdir() loop while the
 * directory was still open:
 *
 *     DIR *d = rb->opendir(dir);
 *     while ((e = rb->readdir(d))) { ... rb->remove(path); ... }
 *     rb->closedir(d);
 *
 * Rockbox's directory iterator holds a cursor into the directory's cluster
 * chain plus cached directory sectors. rb->remove() rewrites those same
 * sectors (marking entries 0xE5 and freeing cluster chains), so the open
 * handle's cached sector and entry index go stale. With long filenames -
 * where one file spans several consecutive 32-byte entries - the iterator
 * then walks into orphaned LFN fragments, and stale cached sectors get
 * written back over updated ones.
 *
 * On a 4000+ file thumbs directory that produced exactly what chkdsk later
 * reported: garbage entry names, "first allocation unit is not valid",
 * "Folder has non-zero file size", and invalid timestamps - and left the
 * FAT free-cluster accounting wrong, which corrupted subsequent iTunes
 * writes and broke syncing.
 *
 * The fix: never mutate a directory while iterating it. Collect a batch of
 * names, CLOSE the directory, then delete. Repeat until a pass finds
 * nothing. Bounded memory, no open handle during any removal.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/

#include "plugin.h"

#define BASE_DIR   "/.rockbox/Adams_Playlists"
#define THUMB_DIR  BASE_DIR "/thumbs"
#define DB_DIR     BASE_DIR "/db"

#define BATCH_MAX  48     /* names collected per pass */
#define NAME_MAX_  96     /* longest filename handled  */

/* One pass: collect up to BATCH_MAX deletable names, close the dir, delete.
 * *found receives how many names were collected. Returns how many were
 * actually removed. */
static int wipe_pass(const char *dir, bool base_rules, bool keep_settings,
                     int *found)
{
    static char names[BATCH_MAX][NAME_MAX_];
    struct dirent *e;
    int count = 0;

    *found = 0;

    DIR *d = rb->opendir(dir);
    if (!d)
        return 0;

    while (count < BATCH_MAX && (e = rb->readdir(d)))
    {
        const char *nm = e->d_name;

        if (nm[0] == '.')
            continue;

        if (base_rules)
        {
            /* subdirectories are handled by their own wipe */
            if (!rb->strcmp(nm, "thumbs") || !rb->strcmp(nm, "db"))
                continue;

            int len = rb->strlen(nm);
            if (keep_settings && len > 4 &&
                (!rb->strcmp(nm + len - 4, ".cfg") ||
                 !rb->strcmp(nm + len - 4, ".ord")))
                continue;                 /* preserve user settings */
        }

        if (rb->strlen(nm) >= NAME_MAX_)
            continue;                     /* too long to buffer safely */

        rb->snprintf(names[count], NAME_MAX_, "%s", nm);
        count++;
    }

    rb->closedir(d);                      /* closed BEFORE any removal */

    *found = count;

    char path[MAX_PATH];
    int removed = 0;
    for (int i = 0; i < count; i++)
    {
        rb->snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (rb->remove(path) == 0)
            removed++;
        rb->yield();
    }
    return removed;
}

/* Repeat passes until nothing deletable is left. Stops if a pass collects
 * names but removes none, so a stubborn file cannot spin forever. */
static int wipe_safe(const char *dir, bool base_rules, bool keep_settings)
{
    int total = 0, found = 0, guard = 0;

    do
    {
        int removed = wipe_pass(dir, base_rules, keep_settings, &found);
        total += removed;

        if (found > 0 && removed == 0)
            break;                        /* cannot make progress */

        if (++guard > 4096)
            break;                        /* hard ceiling, never hang */
    }
    while (found > 0);

    return total;
}

static int wipe_dir(const char *dir)   { return wipe_safe(dir, false, false); }
static int wipe_base(bool keep)        { return wipe_safe(BASE_DIR, true, keep); }

static void clear_sync_stamp(void)
{
    rb->remove(BASE_DIR "/sync.stamp");
    rb->remove(BASE_DIR "/tracks.tmp");
}

static bool confirm(const char *what)
{
    MENUITEM_STRINGLIST(m, "Are you sure?", NULL, "No, cancel", "Yes, clear");
    int sel = 0;
    rb->splashf(HZ, "%s", what);
    return rb->do_menu(&m, &sel, NULL, false) == 1;
}

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;

    if (!rb->dir_exists(BASE_DIR))
    {
        rb->splash(HZ * 2, "No cache found (Adam's Playlists not set up).");
        return PLUGIN_OK;
    }

    MENUITEM_STRINGLIST(menu, "Clear Cache", NULL,
                        "Clear album artwork",
                        "Clear all cache (art + playlists)",
                        "Full reset (also settings)",
                        "How this works",
                        "Exit");
    int sel = 0;

    while (1)
    {
        int choice = rb->do_menu(&menu, &sel, NULL, false);
        int n;
        switch (choice)
        {
            case 0:  /* artwork only */
                if (!confirm("Clear album artwork cache?"))
                    break;
                n = wipe_dir(THUMB_DIR);
                clear_sync_stamp();
                rb->splashf(HZ * 3, "Cleared %d artwork files. "
                            "Art rebuilds on next launch.", n);
                break;

            case 1:  /* art + playlist data, keep settings */
                if (!confirm("Clear art + playlist data? (keeps sort settings)"))
                    break;
                n = wipe_dir(THUMB_DIR);
                n += wipe_dir(DB_DIR);
                n += wipe_base(true);          /* keep .cfg / .ord */
                clear_sync_stamp();
                rb->splashf(HZ * 3, "Cleared %d files. Playlists + art "
                            "rebuild automatically on next launch.", n);
                break;

            case 2:  /* full reset incl. settings */
                if (!confirm("Full reset? This also erases sort settings "
                             "and custom orders."))
                    break;
                n = wipe_dir(THUMB_DIR);
                n += wipe_dir(DB_DIR);
                n += wipe_base(false);         /* delete everything */
                rb->splashf(HZ * 3, "Full reset done (%d files). "
                            "Rebuilds fresh on next launch.", n);
                break;

            case 3:
                rb->splashf(HZ * 5, "Clearing cache never touches your music "
                            "or iTunes. Just reopen Adam's Playlists and it "
                            "rebuilds itself - no manual update needed.");
                break;

            case 4:
            default:
                return PLUGIN_OK;
        }
    }
    return PLUGIN_OK;
}
