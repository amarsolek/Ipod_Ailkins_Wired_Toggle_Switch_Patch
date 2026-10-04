/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2010 Thomas Martitz
 * Plugins category menu with in-device customize (show/hide + reorder).
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "lang.h"
#include "menu.h"
#include "action.h"
#include "settings.h"
#include "rbpaths.h"
#include "root_menu.h"
#include "tree.h"
#include "file.h"
#include "misc.h"
#include "gui/list.h"
#include "icons.h"

enum {
    GAMES,
    APPS,
    DEMOS,
    NUM_CATS
};

static const struct {
    const char *path;
    int id;
    const char *key;
} items[] = {
    { PLUGIN_GAMES_DIR, LANG_PLUGIN_GAMES, "games" },
    { PLUGIN_APPS_DIR,  LANG_PLUGIN_APPS,  "apps"  },
    { PLUGIN_DEMOS_DIR, LANG_PLUGIN_DEMOS, "demos" },
};

#define PLUGIN_MENU_CFG ROCKBOX_DIR "/plugin_menu.cfg"
#define CUSTOMIZE_SENTINEL 0x40000

/* display order (indices into items[]) and per-category visibility */
static int  cat_order[NUM_CATS] = { GAMES, APPS, DEMOS };
static bool cat_shown[NUM_CATS] = { true, true, true };

/* ---------------- config load / save ---------------- */

static int key_to_idx(const char *key)
{
    for (int i = 0; i < NUM_CATS; i++)
        if (!strcmp(items[i].key, key))
            return i;
    return -1;
}

static void load_plugin_menu_cfg(void)
{
    for (int i = 0; i < NUM_CATS; i++)
    {
        cat_order[i] = i;
        cat_shown[i] = true;
    }

    int fd = open(PLUGIN_MENU_CFG, O_RDONLY);
    if (fd < 0)
        return;

    char line[32];
    int n = 0;
    bool seen[NUM_CATS] = { false, false, false };
    while (n < NUM_CATS && read_line(fd, line, sizeof(line)) > 0)
    {
        char *sp = strchr(line, ' ');   /* format: "<key> <0|1>" */
        int shown = 1;
        if (sp)
        {
            *sp = '\0';
            shown = (sp[1] == '0') ? 0 : 1;
        }
        int idx = key_to_idx(line);
        if (idx >= 0 && !seen[idx])
        {
            cat_order[n++] = idx;
            cat_shown[idx] = shown ? true : false;
            seen[idx] = true;
        }
    }
    close(fd);

    for (int i = 0; i < NUM_CATS && n < NUM_CATS; i++)
        if (!seen[i])
            cat_order[n++] = i;
}

static void save_plugin_menu_cfg(void)
{
    int fd = open(PLUGIN_MENU_CFG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    for (int i = 0; i < NUM_CATS; i++)
    {
        int idx = cat_order[i];
        fdprintf(fd, "%s %d\n", items[idx].key, cat_shown[idx] ? 1 : 0);
    }
    close(fd);
}

/* ---------------- browse a category ---------------- */

static void pm_handler(unsigned short id, void *data)
{
    remove_event(id, data);
}

static int plugins_menu(void* param)
{
    intptr_t item = (intptr_t)param;
    int ret;

    static struct browse_context browse = {
        .dirfilter = SHOW_PLUGINS,
        .icon = Icon_Plugin,
    };
    browse.root = items[item].path;
    browse.title = str(items[item].id);

    ret = rockbox_browse(&browse);

    if (ret == GO_TO_PREVIOUS)
        return 0;
    if (ret == GO_TO_PLUGIN)
        add_event(SYS_EVENT_USB_INSERTED, pm_handler);

    return ret;
}

static int menu_callback(int action,
                         const struct menu_item_ex *this_item,
                         struct gui_synclist *this_list)
{
    (void)this_item;
    static int selected = 0;

    if (action == ACTION_ENTER_MENUITEM)
    {
        this_list->selected_item = selected;
        if (!add_event(SYS_EVENT_USB_INSERTED, pm_handler))
        {
            action = ACTION_STD_OK; /* event exists -- reenter menu */
        }
        remove_event(SYS_EVENT_USB_INSERTED, pm_handler);
    }
    else if (action == ACTION_STD_OK)
    {
        selected = gui_synclist_get_sel_pos(this_list);
    }
    return action;
}

#define ITEM_FLAG (MENU_FUNC_CHECK_RETVAL)

MENUITEM_FUNCTION_W_PARAM(games_item, ITEM_FLAG, ID2P(LANG_PLUGIN_GAMES),
                          plugins_menu, (void*)GAMES, NULL, Icon_Folder);
MENUITEM_FUNCTION_W_PARAM(apps_item,  ITEM_FLAG, ID2P(LANG_PLUGIN_APPS),
                          plugins_menu, (void*)APPS,  NULL, Icon_Folder);
MENUITEM_FUNCTION_W_PARAM(demos_item, ITEM_FLAG, ID2P(LANG_PLUGIN_DEMOS),
                          plugins_menu, (void*)DEMOS, NULL, Icon_Folder);

static int customize_ret(void)
{
    return CUSTOMIZE_SENTINEL;
}
MENUITEM_FUNCTION(customize_item, MENU_FUNC_CHECK_RETVAL,
                  "Customize this menu", customize_ret, NULL,
                  Icon_Menu_setting);

static const struct menu_item_ex *cat_items[NUM_CATS] =
    { &games_item, &apps_item, &demos_item };

/* ---------------- customize screen ---------------- */

static const char *cust_get_name(int sel, void *data,
                                 char *buf, size_t buflen)
{
    (void)data;
    int idx = cat_order[sel];
    snprintf(buf, buflen, "%s: %s", str(items[idx].id),
             cat_shown[idx] ? "shown" : "hidden");
    return buf;
}

static int cust_action_cb(int action, struct gui_synclist *lists)
{
    if (action == ACTION_STD_OK)
    {
        int p = gui_synclist_get_sel_pos(lists);
        if (p >= 0 && p < NUM_CATS)
            cat_shown[cat_order[p]] = !cat_shown[cat_order[p]];
        return ACTION_REDRAW;
    }
    else if (action == ACTION_STD_CONTEXT)   /* long press: move up */
    {
        int p = gui_synclist_get_sel_pos(lists);
        if (p >= 0 && p < NUM_CATS)
        {
            int prev = (p == 0) ? NUM_CATS - 1 : p - 1;
            int t = cat_order[p];
            cat_order[p] = cat_order[prev];
            cat_order[prev] = t;
            gui_synclist_select_item(lists, prev);
        }
        return ACTION_REDRAW;
    }
    return action;
}

static void customize_screen(void)
{
    struct simplelist_info info;
    simplelist_info_init(&info,
                         "Plugins menu (SELECT show/hide, hold=move up)",
                         NUM_CATS, NULL);
    info.get_name = cust_get_name;
    info.action_callback = cust_action_cb;
    info.title_icon = Icon_Plugin;
    simplelist_show_list(&info);
    save_plugin_menu_cfg();
}

/* ---------------- dynamic category menu ---------------- */

static const struct menu_callback_with_desc dyn_desc =
    { menu_callback, ID2P(LANG_PLUGINS), Icon_Plugin };
static const struct menu_item_ex *dyn_sub[NUM_CATS + 1];
static struct menu_item_ex dyn_menu;

static void build_dyn_menu(void)
{
    int n = 0;
    for (int i = 0; i < NUM_CATS; i++)
    {
        int idx = cat_order[i];
        if (cat_shown[idx])
            dyn_sub[n++] = cat_items[idx];
    }
    dyn_sub[n++] = &customize_item;

    dyn_menu.flags = MT_MENU | MENU_HAS_DESC | MENU_ITEM_COUNT(n);
    dyn_menu.submenus = dyn_sub;
    dyn_menu.callback_and_desc = &dyn_desc;
}

/* Root-menu entry point (replaces the old static plugin_menu). */
int plugins_screen(void *param)
{
    (void)param;
    int ret;
    static int selected = 0;

    load_plugin_menu_cfg();

    while (1)
    {
        build_dyn_menu();
        ret = do_menu(&dyn_menu, &selected, NULL, false);

        if (ret == CUSTOMIZE_SENTINEL)
        {
            customize_screen();
            continue;
        }
        break;
    }

    switch (ret)
    {
        case GO_TO_PLUGIN:
        case GO_TO_PLAYLIST_VIEWER:
        case GO_TO_WPS:
        case GO_TO_PREVIOUS_MUSIC:
            return ret;
        default:
            return GO_TO_ROOT;
    }
}
