/***************************************************************************
 * clearcache - clear the Adam's Playlists cache (artwork / playlist data)
 *
 * A small maintenance app: wipes the regenerable cache the Adam's_Playlists
 * plugin keeps in /.rockbox/Adams_Playlists, so it rebuilds fresh. It also
 * clears the sync stamp so the next launch auto-rebuilds - you never have to
 * manually "Update from iTunes" afterward.
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

/* delete every regular file directly inside dir; returns count removed */
static int wipe_dir(const char *dir)
{
    DIR *d = rb->opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    char path[MAX_PATH];
    int n = 0;
    while ((e = rb->readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        rb->snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (rb->remove(path) == 0)
            n++;
        rb->yield();
    }
    rb->closedir(d);
    return n;
}

/* remove files in BASE_DIR matching an optional prefix/suffix.
 * keep_settings: if true, leaves *.cfg and *.ord (sort prefs / custom orders) */
static int wipe_base(bool keep_settings)
{
    DIR *d = rb->opendir(BASE_DIR);
    if (!d)
        return 0;
    struct dirent *e;
    char path[MAX_PATH];
    int n = 0;
    while ((e = rb->readdir(d)))
    {
        const char *nm = e->d_name;
        if (nm[0] == '.')
            continue;
        int len = rb->strlen(nm);
        /* skip the subdirectories (thumbs, db) - handled separately */
        if (!rb->strcmp(nm, "thumbs") || !rb->strcmp(nm, "db"))
            continue;
        if (keep_settings && len > 4 &&
            (!rb->strcmp(nm + len - 4, ".cfg") ||
             !rb->strcmp(nm + len - 4, ".ord")))
            continue;                         /* preserve user settings */
        rb->snprintf(path, sizeof(path), BASE_DIR "/%s", nm);
        if (rb->remove(path) == 0)
            n++;
        rb->yield();
    }
    rb->closedir(d);
    return n;
}

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
