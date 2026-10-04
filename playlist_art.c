/***************************************************************************
 * playlist_art v2 - browse iTunes-synced playlists with album art thumbnails
 *
 * v2: reads artwork straight from the iPod's own ArtworkDB / .ithmb files
 * (pre-rendered RGB565 written by iTunes), so store-downloaded artwork,
 * PNGs and progressive JPEGs all work. Falls back to embedded JPEG art,
 * then to cover.jpg/bmp files. Tries several tracks per album.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 ****************************************************************************/

#include "plugin.h"
#include "lib/pluginlib_actions.h"
#include "lib/read_image.h"

#define ITUNESDB_PATH   "/iPod_Control/iTunes/iTunesDB"
#define ARTWORKDB_PATH  "/iPod_Control/Artwork/ArtworkDB"
#define ARTWORK_DIR     "/iPod_Control/Artwork"
#define BASE_DIR        "/.rockbox/Adams_Playlists"
#define THUMB_DIR       BASE_DIR "/thumbs"
#define DB_DIR          BASE_DIR "/db"
#define PL_DIR          "/Playlists"
#define IDX_FILE        BASE_DIR "/playlists6.idx"
#define STAMP_FILE      BASE_DIR "/sync.stamp"
#define TRACKTMP_FILE   BASE_DIR "/tracks.tmp"
#define TMPJPG_FILE     BASE_DIR "/embedded.tmp"

#define THUMB_SIZE      48
#define MAX_PLAYLISTS   256
#define MAX_TRACKS_PL   10000
#define MAX_LIB_TRACKS  60000
#define MAX_ALBUMS      6000
#define MAX_ARTMAP      30000
#define MAX_ITHMB_FILES 24
#define ALBUM_CANDS     3
#define LRU_SLOTS       24
#define RECWIN_SLOTS    24

#define STATUSBAR_H     16
#define HEADER_H        16
#define ROW_H           (THUMB_SIZE + 4)

struct tr_rec {
    char path[160];
    char title[64];
    char artist[48];
    char album[48];             /* album name */
    unsigned long album_hash;
    unsigned char rating;
    unsigned char reserved[1];
    unsigned short track_no;    /* iTunes track number (1-based) */
    unsigned short year;        /* release year */
    unsigned short pad;
    unsigned long date_added;   /* mac timestamp from iTunesDB */
};

struct pl_rec {
    char name[64];
    char m3u8[80];
    unsigned long tracks;
    unsigned long newest;   /* max date_added of member tracks */
    unsigned long order;    /* iTunes order (0 = Recently Added) */
    unsigned long dbidx;    /* index of db/pl_NNN.plart file */
    unsigned long rank;     /* transient sort rank (custom order) */
};

struct thumb_hdr {
    unsigned short w;
    unsigned short h;
};

static const struct button_mapping *plugin_contexts[] = { pla_main_ctx };

static unsigned char *g_buf   = NULL;
static size_t         g_bufsz = 0;

struct idmap_ent { unsigned long id; unsigned long rec; };

struct album_ent {
    unsigned long hash;
    int cand_ct;
    unsigned long cand_rec[ALBUM_CANDS];
    unsigned long long cand_dbid[ALBUM_CANDS];
    unsigned long newest;   /* max date_added across the album */
};

/* one ArtworkDB thumbnail, keyed by the track's 64-bit dbid */
struct art_ent {
    unsigned long long songid;
    unsigned long offset;
    unsigned long size;
    unsigned short w;
    unsigned short h;
    unsigned short fnidx;
    unsigned short pad;
};

static struct idmap_ent *g_idmap;
static struct album_ent *g_albums;
static struct art_ent   *g_artmap;
static int   g_idmap_ct, g_album_ct, g_artmap_ct;
static char  g_ithmb_names[MAX_ITHMB_FILES][64];
static int   g_ithmb_ct;
static unsigned char *g_decbuf;
static size_t         g_decbuf_sz;

static struct pl_rec *g_pls;
static int  g_pl_ct;

struct lru_slot {
    unsigned long album_hash;
    int last_use;
    struct thumb_hdr hdr;
    fb_data *pix;
};
static struct lru_slot g_lru[LRU_SLOTS];
static fb_data *g_lru_pix;
static int g_use_counter;

struct rec_slot {
    int plidx;
    int index;
    struct tr_rec rec;
};
static struct rec_slot g_recwin[RECWIN_SLOTS];

static struct mp3entry g_id3;

/* sync report */
static unsigned long g_sync_dbtracks;   /* tracks listed in iTunesDB   */
static unsigned long g_sync_indexed;    /* tracks we indexed           */
static unsigned long g_sync_capped;     /* dropped: library cap        */
static unsigned long g_sync_unresolved; /* playlist entries not found  */
static unsigned long g_sync_plcapped;   /* dropped: per-playlist cap   */

/* ---------------- helpers ---------------- */

static unsigned long mfnv(const char *str)
{
    const unsigned long p = 16777619;
    unsigned long hash = 0x811C9DC5;
    if (!str)
        return 0;
    while (*str)
        hash = (hash ^ (unsigned char)*str++) * p;
    return hash ? hash : 1;
}

static unsigned long get_u32le(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned long long get_u64le(const unsigned char *p)
{
    return (unsigned long long)get_u32le(p) |
           ((unsigned long long)get_u32le(p + 4) << 32);
}

static unsigned short get_u16le(const unsigned char *p)
{
    return (unsigned short)(p[0] | (p[1] << 8));
}

static bool tag_is(const unsigned char *p, const char *tag)
{
    return p[0] == tag[0] && p[1] == tag[1] && p[2] == tag[2] && p[3] == tag[3];
}

static int read_at(int fd, off_t pos, void *buf, size_t len)
{
    if (rb->lseek(fd, pos, SEEK_SET) != pos)
        return -1;
    return rb->read(fd, buf, len);
}

static void fit_string(char *buf, int maxw)
{
    int w, h;
    rb->lcd_getstringsize(buf, &w, &h);
    while (w > maxw && buf[0])
    {
        int len = rb->strlen(buf);
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80)
            len--;
        buf[len] = '\0';
        rb->lcd_getstringsize(buf, &w, &h);
    }
}

static void sanitize_filename(char *s)
{
    for (; *s; s++)
        if (*s == '/' || *s == '\\' || *s == ':' || *s == '*' || *s == '?' ||
            *s == '"' || *s == '<' || *s == '>' || *s == '|')
            *s = '_';
}

static void ensure_dirs(void)
{
    if (!rb->file_exists(BASE_DIR))  rb->mkdir(BASE_DIR);
    if (!rb->file_exists(THUMB_DIR)) rb->mkdir(THUMB_DIR);
    if (!rb->file_exists(DB_DIR))    rb->mkdir(DB_DIR);
    if (!rb->file_exists(PL_DIR))    rb->mkdir(PL_DIR);
}

/* ---------------- iTunesDB parsing ---------------- */

enum itdb_err {
    ITDB_OK = 0,
    ITDB_ERR_OPEN,
    ITDB_ERR_MAGIC,
    ITDB_ERR_COMPRESSED,
    ITDB_ERR_IO,
};

static void mhod_string(int fd, off_t pos, unsigned long totallen,
                        char *out, size_t outsz)
{
    unsigned char hdr[40];
    out[0] = '\0';
    if (totallen < 40 || read_at(fd, pos, hdr, 40) != 40)
        return;

    unsigned long len = get_u32le(hdr + 28);
    if (len > totallen - 40)
        len = totallen - 40;

    unsigned long units = len / 2;
    if (units > (outsz - 1) / 3)
        units = (outsz - 1) / 3;
    if (units == 0)
        return;

    unsigned char u16[512];
    if (units * 2 > sizeof(u16))
        units = sizeof(u16) / 2;
    if (read_at(fd, pos + 40, u16, units * 2) != (int)(units * 2))
        return;

    unsigned char *end = rb->utf16LEdecode(u16, (unsigned char *)out, units);
    *end = '\0';
}

static void location_to_path(char *s)
{
    char *p;
    for (p = s; *p; p++)
        if (*p == ':')
            *p = '/';
}

static int idmap_cmp(const void *a, const void *b)
{
    unsigned long ia = ((const struct idmap_ent *)a)->id;
    unsigned long ib = ((const struct idmap_ent *)b)->id;
    return (ia > ib) - (ia < ib);
}

static long idmap_find(unsigned long id)
{
    int lo = 0, hi = g_idmap_ct - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (g_idmap[mid].id == id) return (long)g_idmap[mid].rec;
        if (g_idmap[mid].id < id) lo = mid + 1;
        else hi = mid - 1;
    }
    return -1;
}

static void sync_progress(const char *phase, int done, int total)
{
    char line[64];
    rb->lcd_set_foreground(LCD_RGBPACK(230,230,230));
    rb->lcd_set_background(LCD_RGBPACK(0,0,0));
    rb->lcd_clear_display();
    rb->lcd_putsxy(4, LCD_HEIGHT / 2 - 24, "Updating from iTunes...");
    rb->lcd_putsxy(4, LCD_HEIGHT / 2 - 8, phase);
    if (total > 0)
    {
        int w = (LCD_WIDTH - 8) * done / total;
        rb->snprintf(line, sizeof(line), "%d / %d", done, total);
        rb->lcd_putsxy(4, LCD_HEIGHT / 2 + 10, line);
        rb->lcd_drawrect(4, LCD_HEIGHT / 2 + 28, LCD_WIDTH - 8, 10);
        rb->lcd_fillrect(4, LCD_HEIGHT / 2 + 28, w, 10);
    }
    rb->lcd_update();
}

static enum itdb_err parse_tracks(int fd, off_t pos, off_t end)
{
    unsigned char hdr[128];
    struct tr_rec rec;
    char album[64];
    char albumartist[64];

    if (read_at(fd, pos, hdr, 12) != 12 || !tag_is(hdr, "mhlt"))
        return ITDB_ERR_MAGIC;
    unsigned long hdrlen  = get_u32le(hdr + 4);
    unsigned long ntracks = get_u32le(hdr + 8);
    off_t cur = pos + hdrlen;

    g_sync_dbtracks = ntracks;

    int tfd = rb->open(TRACKTMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (tfd < 0)
        return ITDB_ERR_IO;

    unsigned long n;
    for (n = 0; n < ntracks && cur + 40 <= end; n++)
    {
        if (read_at(fd, cur, hdr, sizeof(hdr)) < 40 || !tag_is(hdr, "mhit"))
            break;
        unsigned long ihdr = get_u32le(hdr + 4);
        unsigned long ilen = get_u32le(hdr + 8);
        unsigned long nmhod = get_u32le(hdr + 12);
        unsigned long id = get_u32le(hdr + 16);
        unsigned char rating = hdr[31];
        unsigned long long dbid = 0;
        if (ihdr >= 0x78)
            dbid = get_u64le(hdr + 0x70);
        unsigned long dadded = 0;
        if (ihdr >= 0x6C)
            dadded = get_u32le(hdr + 0x68);
        unsigned long trackno = 0;
        if (ihdr >= 0x30)
            trackno = get_u32le(hdr + 0x2C);
        unsigned long yr = 0;
        if (ihdr >= 0x38)
            yr = get_u32le(hdr + 0x34);

        rb->memset(&rec, 0, sizeof(rec));
        rec.rating = rating / 20;
        rec.date_added = dadded;
        rec.track_no = (unsigned short)trackno;
        rec.year = (unsigned short)yr;
        album[0] = '\0';
        albumartist[0] = '\0';

        off_t mpos = cur + ihdr;
        unsigned long m;
        for (m = 0; m < nmhod && mpos + 16 <= end; m++)
        {
            unsigned char mh[16];
            if (read_at(fd, mpos, mh, 16) != 16 || !tag_is(mh, "mhod"))
                break;
            unsigned long mlen = get_u32le(mh + 8);
            unsigned long mtype = get_u32le(mh + 12);
            if (mlen < 24)
                break;

            if (mtype == 1)
                mhod_string(fd, mpos, mlen, rec.title, sizeof(rec.title));
            else if (mtype == 2)
            {
                mhod_string(fd, mpos, mlen, rec.path, sizeof(rec.path));
                location_to_path(rec.path);
            }
            else if (mtype == 3)
                mhod_string(fd, mpos, mlen, album, sizeof(album));
            else if (mtype == 4)
                mhod_string(fd, mpos, mlen, rec.artist, sizeof(rec.artist));
            else if (mtype == 22)   /* album artist */
                mhod_string(fd, mpos, mlen, albumartist, sizeof(albumartist));

            mpos += mlen;
        }

        if (rec.path[0] && g_idmap_ct >= MAX_LIB_TRACKS)
            g_sync_capped++;
        if (rec.path[0] && g_idmap_ct < MAX_LIB_TRACKS)
        {
            if (album[0])
            {
                char key[160];
                rb->snprintf(key, sizeof(key), "%s\x01%s", album,
                             albumartist[0] ? albumartist : rec.artist);
                rec.album_hash = mfnv(key);
            }
            else
                rec.album_hash = mfnv(rec.artist);
            rb->strlcpy(rec.album, album, sizeof(rec.album));
            if (rec.title[0] == '\0')
            {
                char *base = rb->strrchr(rec.path, '/');
                rb->strlcpy(rec.title, base ? base + 1 : rec.path,
                            sizeof(rec.title));
            }

            rb->write(tfd, &rec, sizeof(rec));
            g_idmap[g_idmap_ct].id = id;
            g_idmap[g_idmap_ct].rec = (unsigned long)g_idmap_ct;
            g_idmap_ct++;

            /* album candidate collection */
            {
                int a;
                for (a = 0; a < g_album_ct; a++)
                    if (g_albums[a].hash == rec.album_hash)
                        break;
                if (a == g_album_ct && g_album_ct < MAX_ALBUMS)
                {
                    rb->memset(&g_albums[a], 0, sizeof(g_albums[a]));
                    g_albums[a].hash = rec.album_hash;
                    g_album_ct++;
                }
                if (a < g_album_ct &&
                    g_albums[a].cand_ct < ALBUM_CANDS)
                {
                    int c = g_albums[a].cand_ct++;
                    g_albums[a].cand_rec[c] =
                        (unsigned long)(g_idmap_ct - 1);
                    g_albums[a].cand_dbid[c] = dbid;
                }
                if (a < g_album_ct && rec.date_added > g_albums[a].newest)
                    g_albums[a].newest = rec.date_added;
            }
        }

        cur += ilen;
        if ((n & 63) == 0)
        {
            sync_progress("Reading library...", n, ntracks);
            rb->yield();
            rb->reset_poweroff_timer();
        }
    }

    rb->close(tfd);
    g_sync_indexed = (unsigned long)g_idmap_ct;
    rb->qsort(g_idmap, g_idmap_ct, sizeof(struct idmap_ent), idmap_cmp);
    return ITDB_OK;
}

/* Recently Added ordering: newest album first, then track number. */
struct recent_ent { unsigned short rank; unsigned short track_no;
                    unsigned long ti; };
static int cmp_recent(const void *a, const void *b)
{
    const struct recent_ent *x = a, *y = b;
    if (x->rank != y->rank)
        return (x->rank > y->rank) - (x->rank < y->rank);
    if (x->track_no != y->track_no)
        return (x->track_no > y->track_no) - (x->track_no < y->track_no);
    return (x->ti > y->ti) - (x->ti < y->ti);
}

static enum itdb_err parse_playlists(int fd, off_t pos, off_t end, int tfd)
{
    unsigned char hdr[32];
    struct tr_rec rec;

    if (read_at(fd, pos, hdr, 12) != 12 || !tag_is(hdr, "mhlp"))
        return ITDB_ERR_MAGIC;
    unsigned long hdrlen = get_u32le(hdr + 4);
    unsigned long npl    = get_u32le(hdr + 8);
    off_t cur = pos + hdrlen;

    g_pl_ct = 0;

    unsigned long n;
    for (n = 0; n < npl && cur + 24 <= end && g_pl_ct < MAX_PLAYLISTS; n++)
    {
        if (read_at(fd, cur, hdr, 24) != 24 || !tag_is(hdr, "mhyp"))
            break;
        unsigned long yhdr  = get_u32le(hdr + 4);
        unsigned long ylen  = get_u32le(hdr + 8);
        unsigned long nmhod = get_u32le(hdr + 12);
        unsigned long nmhip = get_u32le(hdr + 16);
        unsigned char master = hdr[20];

        char plname[64];
        plname[0] = '\0';

        off_t mpos = cur + yhdr;
        unsigned long m;
        for (m = 0; m < nmhod && mpos + 16 <= end; m++)
        {
            unsigned char mh[16];
            if (read_at(fd, mpos, mh, 16) != 16 || !tag_is(mh, "mhod"))
                break;
            unsigned long mlen = get_u32le(mh + 8);
            unsigned long mtype = get_u32le(mh + 12);
            if (mlen < 24)
                break;
            if (mtype == 1)
                mhod_string(fd, mpos, mlen, plname, sizeof(plname));
            mpos += mlen;
        }

        if (master || plname[0] == '\0')
        {
            cur += ylen;
            continue;
        }

        struct pl_rec *pl = &g_pls[g_pl_ct];
        rb->memset(pl, 0, sizeof(*pl));
        rb->strlcpy(pl->name, plname, sizeof(pl->name));
        pl->order = (unsigned long)g_pl_ct + 1;
        pl->dbidx = (unsigned long)g_pl_ct;

        char fname[64];
        rb->strlcpy(fname, plname, sizeof(fname));
        sanitize_filename(fname);
        rb->snprintf(pl->m3u8, sizeof(pl->m3u8), PL_DIR "/%s.m3u8", fname);

        char dbpath[MAX_PATH];
        rb->snprintf(dbpath, sizeof(dbpath), DB_DIR "/pl_%03d.plart", g_pl_ct);

        int m3fd = rb->open(pl->m3u8, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        int dbfd = rb->open(dbpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (m3fd < 0 || dbfd < 0)
        {
            if (m3fd >= 0) rb->close(m3fd);
            if (dbfd >= 0) rb->close(dbfd);
            return ITDB_ERR_IO;
        }
        const char *m3hdr = "#EXTM3U\n";
        rb->write(m3fd, m3hdr, rb->strlen(m3hdr));

        unsigned long added = 0;
        unsigned long i;
        for (i = 0; i < nmhip && mpos + 32 <= end; i++)
        {
            unsigned char ih[32];
            if (read_at(fd, mpos, ih, 32) != 32 || !tag_is(ih, "mhip"))
                break;
            unsigned long ihdr2 = get_u32le(ih + 4);
            unsigned long ilen2 = get_u32le(ih + 8);
            unsigned long trackid = get_u32le(ih + 24);
            if (ilen2 < ihdr2)
                ilen2 = ihdr2;

            long recidx = idmap_find(trackid);
            if (recidx < 0 && trackid != 0)
                g_sync_unresolved++;
            else if (recidx >= 0 && added >= MAX_TRACKS_PL)
                g_sync_plcapped++;
            if (recidx >= 0 && added < MAX_TRACKS_PL)
            {
                if (read_at(tfd, (off_t)recidx * sizeof(rec), &rec,
                            sizeof(rec)) == (int)sizeof(rec))
                {
                    rb->write(dbfd, &rec, sizeof(rec));
                    rb->write(m3fd, rec.path, rb->strlen(rec.path));
                    rb->write(m3fd, "\n", 1);
                    if (rec.date_added > pl->newest)
                        pl->newest = rec.date_added;
                    added++;
                }
            }
            mpos += ilen2;
        }

        rb->close(m3fd);
        rb->close(dbfd);

        if (added > 0)
        {
            pl->tracks = added;
            g_pl_ct++;
        }
        else
        {
            rb->remove(pl->m3u8);
            rb->remove(dbpath);
        }

        sync_progress("Reading playlists...", (int)(n + 1), (int)npl);
        rb->yield();
        cur += ylen;
    }

    /* virtual "Recently Added" playlist: newest tracks in the library */
    if (g_pl_ct < MAX_PLAYLISTS)
    {
        /* virtual "Recently Added": newest albums first, each album's
         * songs ordered by track number (starting at 1). */
#define MAX_RECENT_ALBUMS 60
#define MAX_RECENT_TRACKS 800
        static struct { unsigned long hash; unsigned long newest; }
            sel_alb[MAX_RECENT_ALBUMS];
        static struct recent_ent sel[MAX_RECENT_TRACKS];
        int nalb = 0, nsel = 0, k, j;

        /* pick the newest MAX_RECENT_ALBUMS albums by date added */
        for (k = 0; k < g_album_ct; k++)
        {
            unsigned long d = g_albums[k].newest;
            if (d == 0)
                continue;
            if (nalb < MAX_RECENT_ALBUMS)
            {
                sel_alb[nalb].hash = g_albums[k].hash;
                sel_alb[nalb].newest = d;
                nalb++;
            }
            else
            {
                int mi = 0;
                for (j = 1; j < MAX_RECENT_ALBUMS; j++)
                    if (sel_alb[j].newest < sel_alb[mi].newest) mi = j;
                if (d > sel_alb[mi].newest)
                {
                    sel_alb[mi].hash = g_albums[k].hash;
                    sel_alb[mi].newest = d;
                }
            }
        }
        /* order the selected albums newest first */
        for (k = 1; k < nalb; k++)
        {
            unsigned long h = sel_alb[k].hash, d = sel_alb[k].newest;
            for (j = k - 1; j >= 0 && sel_alb[j].newest < d; j--)
                sel_alb[j + 1] = sel_alb[j];
            sel_alb[j + 1].hash = h;
            sel_alb[j + 1].newest = d;
        }

        /* one library pass: collect tracks belonging to those albums */
        {
            off_t tsz = rb->lseek(tfd, 0, SEEK_END);
            long total = (long)(tsz / (off_t)sizeof(struct tr_rec));
            long ti;
            rb->lseek(tfd, 0, SEEK_SET);
            for (ti = 0; ti < total && nsel < MAX_RECENT_TRACKS; ti++)
            {
                if (rb->read(tfd, &rec, sizeof(rec)) != (int)sizeof(rec))
                    break;
                for (j = 0; j < nalb; j++)
                    if (sel_alb[j].hash == rec.album_hash)
                        break;
                if (j < nalb)
                {
                    sel[nsel].rank = (unsigned short)j;
                    sel[nsel].track_no = rec.track_no;
                    sel[nsel].ti = (unsigned long)ti;
                    nsel++;
                }
                if ((ti & 1023) == 0)
                    rb->yield();
            }
        }

        /* newest album first, then track number, then file order */
        if (nsel > 1)
            rb->qsort(sel, nsel, sizeof(sel[0]), cmp_recent);

        if (nsel > 0)
        {
            struct pl_rec *pl = &g_pls[g_pl_ct];
            rb->memset(pl, 0, sizeof(*pl));
            rb->strlcpy(pl->name, "Recently Added", sizeof(pl->name));
            rb->strcpy(pl->m3u8, PL_DIR "/Recently Added.m3u8");
            pl->order = 0;
            pl->dbidx = (unsigned long)g_pl_ct;
            pl->newest = (nalb > 0) ? sel_alb[0].newest : 0;
            char dbpath2[MAX_PATH];
            rb->snprintf(dbpath2, sizeof(dbpath2),
                         DB_DIR "/pl_%03d.plart", g_pl_ct);
            int m3fd2 = rb->open(pl->m3u8, O_WRONLY|O_CREAT|O_TRUNC, 0666);
            int dbfd2 = rb->open(dbpath2, O_WRONLY|O_CREAT|O_TRUNC, 0666);
            if (m3fd2 >= 0 && dbfd2 >= 0)
            {
                rb->write(m3fd2, "#EXTM3U\n", 8);
                for (k = 0; k < nsel; k++)
                {
                    if (read_at(tfd, (off_t)sel[k].ti * sizeof(rec), &rec,
                                sizeof(rec)) != (int)sizeof(rec))
                        continue;
                    rb->write(dbfd2, &rec, sizeof(rec));
                    rb->write(m3fd2, rec.path, rb->strlen(rec.path));
                    rb->write(m3fd2, "\n", 1);
                    pl->tracks++;
                }
            }
            if (m3fd2 >= 0) rb->close(m3fd2);
            if (dbfd2 >= 0) rb->close(dbfd2);
            if (pl->tracks > 0)
                g_pl_ct++;
            else
            {
                rb->remove(pl->m3u8);
                rb->remove(dbpath2);
            }
        }
    }

    int xfd = rb->open(IDX_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (xfd < 0)
        return ITDB_ERR_IO;
    unsigned long ct = (unsigned long)g_pl_ct;
    rb->write(xfd, &ct, sizeof(ct));
    rb->write(xfd, g_pls, sizeof(struct pl_rec) * g_pl_ct);
    rb->close(xfd);
    return ITDB_OK;
}

static enum itdb_err parse_itunesdb(void)
{
    unsigned char hdr[16];
    enum itdb_err err = ITDB_OK;

    int fd = rb->open(ITUNESDB_PATH, O_RDONLY);
    if (fd < 0)
        return ITDB_ERR_OPEN;

    off_t fsize = rb->lseek(fd, 0, SEEK_END);

    if (read_at(fd, 0, hdr, 12) != 12 || !tag_is(hdr, "mhbd"))
    {
        rb->close(fd);
        return ITDB_ERR_MAGIC;
    }
    unsigned long bhdr = get_u32le(hdr + 4);

    unsigned char z[2];
    if (read_at(fd, bhdr, z, 2) == 2 && z[0] == 0x78)
    {
        rb->close(fd);
        return ITDB_ERR_COMPRESSED;
    }

    g_idmap_ct = 0;
    g_album_ct = 0;
    g_sync_dbtracks = g_sync_indexed = 0;
    g_sync_capped = g_sync_unresolved = g_sync_plcapped = 0;

    off_t cur = bhdr;
    off_t tracks_pos = -1, pl_pos = -1, pl_pos_alt = -1;
    while (cur + 16 <= fsize)
    {
        if (read_at(fd, cur, hdr, 16) != 16 || !tag_is(hdr, "mhsd"))
            break;
        unsigned long shdr = get_u32le(hdr + 4);
        unsigned long slen = get_u32le(hdr + 8);
        unsigned long stype = get_u32le(hdr + 12);
        if (stype == 1 && tracks_pos < 0)
            tracks_pos = cur + shdr;
        else if (stype == 2 && pl_pos < 0)
            pl_pos = cur + shdr;
        else if (stype == 3 && pl_pos_alt < 0)
            pl_pos_alt = cur + shdr;
        if (slen == 0)
            break;
        cur += slen;
    }

    if (tracks_pos < 0)
    {
        rb->close(fd);
        return ITDB_ERR_MAGIC;
    }

    err = parse_tracks(fd, tracks_pos, fsize);
    if (err == ITDB_OK)
    {
        if (pl_pos < 0)
            pl_pos = pl_pos_alt;
        if (pl_pos < 0)
            err = ITDB_ERR_MAGIC;
        else
        {
            int tfd = rb->open(TRACKTMP_FILE, O_RDONLY);
            if (tfd < 0)
                err = ITDB_ERR_IO;
            else
            {
                err = parse_playlists(fd, pl_pos, fsize, tfd);
                rb->close(tfd);
            }
        }
    }

    rb->close(fd);
    return err;
}

/* ---------------- ArtworkDB parsing (v2) ---------------- */

static int artmap_cmp(const void *a, const void *b)
{
    unsigned long long ia = ((const struct art_ent *)a)->songid;
    unsigned long long ib = ((const struct art_ent *)b)->songid;
    return (ia > ib) - (ia < ib);
}

static const struct art_ent *artmap_find(unsigned long long songid)
{
    int lo = 0, hi = g_artmap_ct - 1;
    if (songid == 0)
        return NULL;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (g_artmap[mid].songid == songid) return &g_artmap[mid];
        if (g_artmap[mid].songid < songid) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* register an ithmb filename, return its index or -1 */
static int ithmb_name_idx(const char *name)
{
    int i;
    for (i = 0; i < g_ithmb_ct; i++)
        if (!rb->strcmp(g_ithmb_names[i], name))
            return i;
    if (g_ithmb_ct >= MAX_ITHMB_FILES)
        return -1;
    rb->strlcpy(g_ithmb_names[g_ithmb_ct], name,
                sizeof(g_ithmb_names[0]));
    return g_ithmb_ct++;
}

/* Heuristically extract "FNNNN_N.ithmb" from an mhod payload that may be
   ASCII or UTF-16LE. Returns full path in out. */
static bool find_ithmb_name(int fd, off_t pos, unsigned long len,
                            char *out, size_t outsz)
{
    unsigned char raw[256];
    char ascii[128];
    unsigned long i;
    int n = 0;

    int got;
    if (len > sizeof(raw))
        len = sizeof(raw);
    got = read_at(fd, pos, raw, len);
    if (got < 12)
        return false;
    len = (unsigned long)got;

    for (i = 0; i < len && n < (int)sizeof(ascii) - 1; i++)
    {
        unsigned char c = raw[i];
        if (c == 0)
            continue;                 /* skip UTF-16 high bytes */
        if (c >= 32 && c < 127)
            ascii[n++] = (char)c;
        else
            ascii[n++] = ' ';
    }
    ascii[n] = '\0';

    /* find ".ithmb" manually (no strcasestr in plugin api guarantee) */
    int pos_end = -1;
    for (i = 0; (int)i + 6 <= n; i++)
    {
        if (ascii[i] == '.' &&
            (ascii[i+1] == 'i' || ascii[i+1] == 'I') &&
            (ascii[i+2] == 't' || ascii[i+2] == 'T') &&
            (ascii[i+3] == 'h' || ascii[i+3] == 'H') &&
            (ascii[i+4] == 'm' || ascii[i+4] == 'M') &&
            (ascii[i+5] == 'b' || ascii[i+5] == 'B'))
        {
            pos_end = (int)i + 6;
            break;
        }
    }
    if (pos_end < 0)
        return false;

    int start = pos_end - 6;
    while (start > 0)
    {
        char c = ascii[start - 1];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
            start--;
        else
            break;
    }
    ascii[pos_end] = '\0';

    rb->snprintf(out, outsz, ARTWORK_DIR "/%s", ascii + start);
    return true;
}

/* returns number of artwork entries found, or -1 on error */
static int parse_artworkdb(void)
{
    unsigned char hdr[40];

    g_artmap_ct = 0;
    g_ithmb_ct = 0;

    int fd = rb->open(ARTWORKDB_PATH, O_RDONLY);
    if (fd < 0)
        return -1;

    off_t fsize = rb->lseek(fd, 0, SEEK_END);

    if (read_at(fd, 0, hdr, 12) != 12 || !tag_is(hdr, "mhfd"))
    {
        rb->close(fd);
        return -1;
    }
    unsigned long fhdr = get_u32le(hdr + 4);

    /* find mhsd type 1 (image list) */
    off_t cur = fhdr;
    off_t imglist = -1;
    while (cur + 16 <= fsize)
    {
        if (read_at(fd, cur, hdr, 16) != 16 || !tag_is(hdr, "mhsd"))
            break;
        unsigned long shdr = get_u32le(hdr + 4);
        unsigned long slen = get_u32le(hdr + 8);
        unsigned long stype = get_u32le(hdr + 12);
        if (stype == 1)
        {
            imglist = cur + shdr;
            break;
        }
        if (slen == 0)
            break;
        cur += slen;
    }
    if (imglist < 0)
    {
        rb->close(fd);
        return 0;
    }

    if (read_at(fd, imglist, hdr, 12) != 12 || !tag_is(hdr, "mhli"))
    {
        rb->close(fd);
        return -1;
    }
    unsigned long lhdr = get_u32le(hdr + 4);
    unsigned long nimg = get_u32le(hdr + 8);
    cur = imglist + lhdr;

    unsigned long n;
    for (n = 0; n < nimg && cur + 28 <= fsize; n++)
    {
        if (read_at(fd, cur, hdr, 28) != 28 || !tag_is(hdr, "mhii"))
            break;
        unsigned long ihdr = get_u32le(hdr + 4);
        unsigned long ilen = get_u32le(hdr + 8);
        unsigned long nchild = get_u32le(hdr + 12);
        unsigned long long songid = get_u64le(hdr + 20);

        /* walk child mhods; type 2 contains an mhni thumbnail */
        struct art_ent best;
        bool have_best = false;
        rb->memset(&best, 0, sizeof(best));

        off_t mpos = cur + ihdr;
        unsigned long m;
        for (m = 0; m < nchild && mpos + 16 <= fsize; m++)
        {
            unsigned char mh[16];
            if (read_at(fd, mpos, mh, 16) != 16 || !tag_is(mh, "mhod"))
                break;
            unsigned long mhdr = get_u32le(mh + 4);
            unsigned long mlen = get_u32le(mh + 8);
            unsigned int mtype = get_u16le(mh + 12);
            if (mlen < 16 || mlen < mhdr)
                break;

            if (mtype == 2 && mpos + (off_t)mhdr + 36 <= fsize)
            {
                unsigned char nh[36];
                if (read_at(fd, mpos + mhdr, nh, 36) == 36 &&
                    tag_is(nh, "mhni"))
                {
                    unsigned long nhdr = get_u32le(nh + 4);
                    unsigned long nlen = get_u32le(nh + 8);
                    unsigned long ioff = get_u32le(nh + 20);
                    unsigned long isz  = get_u32le(nh + 24);
                    unsigned short ih_ = get_u16le(nh + 32);
                    unsigned short iw_ = get_u16le(nh + 34);

                    if (iw_ >= 32 && iw_ <= 1024 &&
                        ih_ >= 32 && ih_ <= 1024 &&
                        isz > 0 && isz <= 4u * 1024 * 1024)
                    {
                        char fpath[80];
                        (void)nlen;
                        if (find_ithmb_name(fd, mpos + mhdr + nhdr,
                                            256, fpath, sizeof(fpath)))
                        {
                            int fni = ithmb_name_idx(fpath);
                            if (fni >= 0)
                            {
                                bool better;
                                if (!have_best)
                                    better = true;
                                else
                                {
                                    bool bfits = best.w >= THUMB_SIZE &&
                                                 best.h >= THUMB_SIZE;
                                    bool nfits = iw_ >= THUMB_SIZE &&
                                                 ih_ >= THUMB_SIZE;
                                    if (nfits && !bfits)
                                        better = true;
                                    else if (nfits == bfits)
                                        better = ((unsigned long)iw_ * ih_ <
                                            (unsigned long)best.w * best.h)
                                            == nfits;
                                    else
                                        better = false;
                                }
                                if (better)
                                {
                                    best.songid = songid;
                                    best.offset = ioff;
                                    best.size = isz;
                                    best.w = iw_;
                                    best.h = ih_;
                                    best.fnidx = (unsigned short)fni;
                                    have_best = true;
                                }
                            }
                        }
                    }
                }
            }
            mpos += mlen;
        }

        if (have_best && g_artmap_ct < MAX_ARTMAP)
            g_artmap[g_artmap_ct++] = best;

        if (ilen < ihdr)
            ilen = ihdr;
        cur += ilen;

        if ((n & 127) == 0)
        {
            sync_progress("Reading artwork index...", n, nimg);
            rb->yield();
            rb->reset_poweroff_timer();
        }
    }

    rb->close(fd);
    rb->qsort(g_artmap, g_artmap_ct, sizeof(struct art_ent), artmap_cmp);
    return g_artmap_ct;
}

/* ---------------- thumbnails ---------------- */

static void thumb_path(unsigned long hash, char *buf, size_t bufsz)
{
    rb->snprintf(buf, bufsz, THUMB_DIR "/%08lx.raw", hash);
}

/* nearest-neighbour scale, RGB565 -> RGB565 */
static void scale_rgb565(const unsigned short *src, int sw, int sh,
                         int sstride, unsigned short *dst, int dw, int dh)
{
    int x, y;
    for (y = 0; y < dh; y++)
    {
        const unsigned short *srow = src + ((long)y * sh / dh) * sstride;
        unsigned short *drow = dst + (long)y * dw;
        for (x = 0; x < dw; x++)
            drow[x] = srow[(long)x * sw / dw];
    }
}

static bool save_thumb_file(unsigned long hash, const unsigned short *pix,
                            int w, int h)
{
    char tpath[MAX_PATH];
    struct thumb_hdr th;
    th.w = (unsigned short)w;
    th.h = (unsigned short)h;
    thumb_path(hash, tpath, sizeof(tpath));
    int fd = rb->open(tpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return false;
    rb->write(fd, &th, sizeof(th));
    rb->write(fd, pix, (size_t)w * h * 2);
    rb->close(fd);
    return true;
}

/* v2: load pre-rendered RGB565 art straight from an .ithmb file */
static bool build_thumb_from_ithmb(unsigned long hash,
                                   const struct art_ent *ae)
{
    if (ae->fnidx >= g_ithmb_ct)
        return false;
    if (ae->size + THUMB_SIZE * THUMB_SIZE * 2 + 64 > g_decbuf_sz)
        return false;

    int fd = rb->open(g_ithmb_names[ae->fnidx], O_RDONLY);
    if (fd < 0)
        return false;

    unsigned short *src = (unsigned short *)g_decbuf;
    int got = read_at(fd, (off_t)ae->offset, src, ae->size);
    rb->close(fd);
    if (got != (int)ae->size)
        return false;

    int w = ae->w, h = ae->h;
    long stride = w;
    if (h > 0 && (long)ae->size >= (long)w * h * 2)
    {
        long rowbytes = (long)ae->size / h;
        if (rowbytes >= w * 2)
            stride = rowbytes / 2;
    }

    /* aspect-fit into THUMB_SIZE box */
    int dw = THUMB_SIZE, dh = THUMB_SIZE;
    if (w > h)
        dh = MAX(1, THUMB_SIZE * h / w);
    else if (h > w)
        dw = MAX(1, THUMB_SIZE * w / h);

    unsigned short *dst = (unsigned short *)
        ALIGN_UP((uintptr_t)(g_decbuf + ae->size + 4), 4);
    scale_rgb565(src, w, h, stride, dst, dw, dh);

    return save_thumb_file(hash, dst, dw, dh);
}

static bool extract_embedded(const char *audio, off_t pos, int size)
{
    int in = rb->open(audio, O_RDONLY);
    if (in < 0)
        return false;
    int out = rb->open(TMPJPG_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0)
    {
        rb->close(in);
        return false;
    }
    bool ok = (rb->lseek(in, pos, SEEK_SET) == pos);
    int left = size;
    while (ok && left > 0)
    {
        int chunk = MIN(left, 16384);
        int got = rb->read(in, g_decbuf, chunk);
        if (got <= 0) { ok = false; break; }
        if (rb->write(out, g_decbuf, got) != got) { ok = false; break; }
        left -= got;
        rb->yield();
    }
    rb->close(in);
    rb->close(out);
    return ok && left == 0;
}

/* decode embedded JPEG or folder cover file for one track */
static bool build_thumb_decoded(unsigned long hash,
                                const struct tr_rec *rec)
{
    char srcart[MAX_PATH];
    struct bitmap bm;

    if (!rb->file_exists(rec->path))
        return false;
    if (!rb->get_metadata(&g_id3, -1, rec->path))
        return false;

    srcart[0] = '\0';
    bool tmp_used = false;

#ifdef HAVE_JPEG
    if (g_id3.has_embedded_albumart &&
        (g_id3.albumart.type & 0xFF) == AA_TYPE_JPG &&
        g_id3.albumart.size > 0 && g_id3.albumart.size < (1 << 22))
    {
        if (extract_embedded(rec->path, g_id3.albumart.pos,
                             g_id3.albumart.size))
        {
            rb->strlcpy(srcart, TMPJPG_FILE, sizeof(srcart));
            tmp_used = true;
        }
    }
#endif

    if (srcart[0] == '\0')
    {
        if (!rb->search_albumart_files(&g_id3, ":", srcart, sizeof(srcart)))
            return false;
    }

    rb->memset(&bm, 0, sizeof(bm));
    bm.width = THUMB_SIZE;
    bm.height = THUMB_SIZE;
    bm.data = g_decbuf;

    int ret = read_image_file(srcart, &bm, (int)g_decbuf_sz,
                              FORMAT_NATIVE | FORMAT_DITHER |
                              FORMAT_RESIZE | FORMAT_KEEP_ASPECT, NULL);
    if (tmp_used)
        rb->remove(TMPJPG_FILE);
    if (ret <= 0)
        return false;

    return save_thumb_file(hash, (unsigned short *)bm.data,
                           bm.width, bm.height);
}

/* try all sources for one album; true if a thumb file now exists */
/* a thumb counts as present only if it has real data (guards against
 * 0-byte / truncated files left by an interrupted rebuild) */
static bool thumb_valid(const char *path)
{
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0)
        return false;
    off_t sz = rb->lseek(fd, 0, SEEK_END);
    rb->close(fd);
    return sz > (off_t)sizeof(struct thumb_hdr);
}

static bool build_thumb(const struct album_ent *al, int tfd, bool force)
{
    char tpath[MAX_PATH];
    struct tr_rec rec;
    int c;

    thumb_path(al->hash, tpath, sizeof(tpath));
    if (!force && thumb_valid(tpath))
        return true;
    rb->remove(tpath);   /* drop any empty/partial file before rebuilding */

    /* 1: ArtworkDB (covers store-downloaded art, PNG, everything) */
    for (c = 0; c < al->cand_ct; c++)
    {
        const struct art_ent *ae = artmap_find(al->cand_dbid[c]);
        if (ae && build_thumb_from_ithmb(al->hash, ae))
            return true;
    }

    /* 2: embedded JPEG / folder cover files */
    for (c = 0; c < al->cand_ct; c++)
    {
        if (read_at(tfd, (off_t)al->cand_rec[c] * sizeof(rec), &rec,
                    sizeof(rec)) == (int)sizeof(rec)
            && build_thumb_decoded(al->hash, &rec))
            return true;
    }

    return false;
}

static void build_all_thumbs(bool force)
{
    int tfd = rb->open(TRACKTMP_FILE, O_RDONLY);
    if (tfd < 0)
        return;

    int a;
    for (a = 0; a < g_album_ct; a++)
    {
        sync_progress("Caching album art...", a, g_album_ct);
        rb->reset_poweroff_timer();
        rb->yield();
        build_thumb(&g_albums[a], tfd, force);
    }
    rb->close(tfd);
}

/* ---------------- sync stamp ---------------- */

struct db_stamp { off_t size; unsigned long words[4]; };

static bool get_db_fingerprint(struct db_stamp *st)
{
    unsigned char buf[16];
    int fd = rb->open(ITUNESDB_PATH, O_RDONLY);
    if (fd < 0)
        return false;
    rb->memset(st, 0, sizeof(*st));
    st->size = rb->lseek(fd, 0, SEEK_END);
    if (read_at(fd, 0x70, buf, 16) == 16)
    {
        st->words[0] = get_u32le(buf);
        st->words[1] = get_u32le(buf + 4);
        st->words[2] = get_u32le(buf + 8);
        st->words[3] = get_u32le(buf + 12);
    }
    rb->close(fd);
    return true;
}

static bool sync_needed(void)
{
    struct db_stamp cur, saved;
    if (!rb->file_exists(IDX_FILE))
        return true;
    if (!get_db_fingerprint(&cur))
        return false;
    int fd = rb->open(STAMP_FILE, O_RDONLY);
    if (fd < 0)
        return true;
    int got = rb->read(fd, &saved, sizeof(saved));
    rb->close(fd);
    if (got != (int)sizeof(saved))
        return true;
    return rb->memcmp(&cur, &saved, sizeof(cur)) != 0;
}

static void save_stamp(void)
{
    struct db_stamp cur;
    if (!get_db_fingerprint(&cur))
        return;
    int fd = rb->open(STAMP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    rb->write(fd, &cur, sizeof(cur));
    rb->close(fd);
}

/* ---------------- sync ---------------- */

/* delete all regular files in a directory (used to clear the thumb cache) */
static void wipe_dir_files(const char *dir)
{
    DIR *d = rb->opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    char path[MAX_PATH];
    while ((e = rb->readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        rb->snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        rb->remove(path);
        rb->yield();
    }
    rb->closedir(d);
}

/* remove old-version playlists*.idx files (keep only the current one) */
static void clean_stale_idx(void)
{
    const char *cur = rb->strrchr(IDX_FILE, '/');
    cur = cur ? cur + 1 : IDX_FILE;
    DIR *d = rb->opendir(BASE_DIR);
    if (!d)
        return;
    struct dirent *e;
    char path[MAX_PATH];
    while ((e = rb->readdir(d)))
    {
        int n = rb->strlen(e->d_name);
        if (n > 4 && rb->strncmp(e->d_name, "playlists", 9) == 0 &&
            rb->strcmp(e->d_name + n - 4, ".idx") == 0 &&
            rb->strcmp(e->d_name, cur) != 0)
        {
            rb->snprintf(path, sizeof(path), BASE_DIR "/%s", e->d_name);
            rb->remove(path);
        }
    }
    rb->closedir(d);
}

static bool do_sync(bool force_thumbs)
{
    enum itdb_err err;

    ensure_dirs();
    clean_stale_idx();
    if (force_thumbs)               /* Full rebuild: drop orphaned art too */
        wipe_dir_files(THUMB_DIR);
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
    sync_progress("Reading iTunesDB...", 0, 0);

    err = parse_itunesdb();
    if (err == ITDB_OK)
    {
        parse_artworkdb();      /* optional; -1/0 just means no ArtworkDB */
        build_all_thumbs(force_thumbs);
        save_stamp();
        rb->remove(TRACKTMP_FILE);
    }
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif

    switch (err)
    {
        case ITDB_OK:
            rb->splashf(HZ * 2, "Synced %lu tracks, %d playlists",
                        g_sync_indexed, g_pl_ct);
            if (g_sync_capped || g_sync_unresolved || g_sync_plcapped ||
                g_sync_indexed < g_sync_dbtracks)
                rb->splashf(HZ * 4, "Skipped: %lu over cap, %lu no file, "
                            "%lu over playlist cap, %lu unread",
                            g_sync_capped, g_sync_unresolved, g_sync_plcapped,
                            g_sync_dbtracks > g_sync_indexed + g_sync_capped ?
                            g_sync_dbtracks - g_sync_indexed - g_sync_capped
                            : 0);
            return true;
        case ITDB_ERR_OPEN:
            rb->splashf(HZ * 3, "No iTunesDB found. Sync this iPod "
                                "with iTunes first.");
            break;
        case ITDB_ERR_COMPRESSED:
            rb->splashf(HZ * 4, "This iTunesDB is compressed (iTunesCDB) "
                                "- not supported yet.");
            break;
        default:
            rb->splashf(HZ * 3, "Could not parse iTunesDB (error %d)",
                        (int)err);
            break;
    }
    return false;
}

/* ---------------- browse data ---------------- */

static bool load_index(void)
{
    unsigned long ct = 0;
    int fd = rb->open(IDX_FILE, O_RDONLY);
    if (fd < 0)
        return false;
    if (rb->read(fd, &ct, sizeof(ct)) != (int)sizeof(ct) ||
        ct > MAX_PLAYLISTS)
    {
        rb->close(fd);
        return false;
    }
    g_pl_ct = (int)ct;
    int want = (int)(sizeof(struct pl_rec) * ct);
    bool ok = rb->read(fd, g_pls, want) == want;
    rb->close(fd);
    return ok;
}

/* ---------------- playlist sorting ---------------- */

#define SORT_CFG BASE_DIR "/sort.cfg"
#define ORD_FILE BASE_DIR "/custom.ord"
enum { PA_SORT_NEWEST = 0, PA_SORT_ALPHA, PA_SORT_ITUNES, PA_SORT_CUSTOM };
static int g_sort_mode = PA_SORT_NEWEST;

/* save current on-screen order as the custom order (keyed by name) */
static void save_custom_order(void)
{
    int i;
    int fd = rb->open(ORD_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    for (i = 0; i < g_pl_ct; i++)
        rb->write(fd, g_pls[i].name, sizeof(g_pls[0].name));
    rb->close(fd);
}

/* assign ranks from the saved custom order; unknown names go last,
   keeping their iTunes order */
static void load_custom_ranks(void)
{
    char name[64];
    int i, pos = 0;

    for (i = 0; i < g_pl_ct; i++)
        g_pls[i].rank = 0x10000 + g_pls[i].order;

    int fd = rb->open(ORD_FILE, O_RDONLY);
    if (fd < 0)
        return;
    while (rb->read(fd, name, sizeof(name)) == (int)sizeof(name) &&
           pos < MAX_PLAYLISTS)
    {
        name[sizeof(name) - 1] = '\0';
        for (i = 0; i < g_pl_ct; i++)
            if (!rb->strcmp(g_pls[i].name, name))
            {
                g_pls[i].rank = (unsigned long)pos;
                break;
            }
        pos++;
    }
    rb->close(fd);
}

static int plsort_cmp(const void *a, const void *b)
{
    const struct pl_rec *pa = (const struct pl_rec *)a;
    const struct pl_rec *pb = (const struct pl_rec *)b;
    switch (g_sort_mode)
    {
        case PA_SORT_ALPHA:
            /* keep Recently Added pinned on top */
            if (pa->order == 0) return -1;
            if (pb->order == 0) return 1;
            return rb->strcasecmp(pa->name, pb->name);
        case PA_SORT_ITUNES:
            return (pa->order > pb->order) - (pa->order < pb->order);
        case PA_SORT_CUSTOM:
            return (pa->rank > pb->rank) - (pa->rank < pb->rank);
        default: /* PA_SORT_NEWEST */
            return (pb->newest > pa->newest) - (pb->newest < pa->newest);
    }
}

static void apply_sort(void)
{
    if (g_sort_mode == PA_SORT_CUSTOM)
        load_custom_ranks();
    if (g_pl_ct > 1)
        rb->qsort(g_pls, g_pl_ct, sizeof(struct pl_rec), plsort_cmp);
}

static void load_sort(void)
{
    unsigned char b = 0;
    int fd = rb->open(SORT_CFG, O_RDONLY);
    if (fd < 0)
        return;
    if (rb->read(fd, &b, 1) == 1 && b <= PA_SORT_CUSTOM)
        g_sort_mode = b;
    rb->close(fd);
}

static void save_sort(void)
{
    unsigned char b = (unsigned char)g_sort_mode;
    int fd = rb->open(SORT_CFG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    rb->write(fd, &b, 1);
    rb->close(fd);
}

static const struct tr_rec *get_track(int plidx, int index)
{
    int i, slot = -1;
    for (i = 0; i < RECWIN_SLOTS; i++)
    {
        if (g_recwin[i].index == index && g_recwin[i].plidx == plidx)
            return &g_recwin[i].rec;
        if (g_recwin[i].index < 0 && slot < 0)
            slot = i;
    }
    if (slot < 0)
        slot = index % RECWIN_SLOTS;

    char dbpath[MAX_PATH];
    rb->snprintf(dbpath, sizeof(dbpath), DB_DIR "/pl_%03d.plart",
                 (int)g_pls[plidx].dbidx);
    int fd = rb->open(dbpath, O_RDONLY);
    if (fd < 0)
        return NULL;
    int got = read_at(fd, (off_t)index * sizeof(struct tr_rec),
                      &g_recwin[slot].rec, sizeof(struct tr_rec));
    rb->close(fd);
    if (got != (int)sizeof(struct tr_rec))
        return NULL;
    g_recwin[slot].plidx = plidx;
    g_recwin[slot].index = index;
    return &g_recwin[slot].rec;
}

/* ---------------- song sort (within a playlist) ---------------- */

#define SONGSORT_CFG BASE_DIR "/songsort.cfg"
enum { SS_PLAYLIST = 0, SS_ALBUM, SS_ADDED, SS_YEAR,
       SS_TITLE_AZ, SS_TITLE_ZA, SS_ARTIST, SS_ALBUM_ZA, SS_CUSTOM,
       SS_ARTIST_ZA };
static int g_song_sort = SS_PLAYLIST;   /* global default (all playlists) */
static int g_active_sort = SS_PLAYLIST; /* effective mode for current build */

/* per-playlist sort overrides (playlist name -> mode); absent = use global */
#define PLSORT_CFG BASE_DIR "/plsort.cfg"
struct pp_ent { char name[64]; unsigned char mode; };
static struct pp_ent g_pp[MAX_PLAYLISTS];
static int g_pp_ct = 0;

static int pp_get_mode(const char *name)
{
    int i;
    for (i = 0; i < g_pp_ct; i++)
        if (!rb->strcmp(g_pp[i].name, name))
            return g_pp[i].mode;
    return -1;
}
static void pp_save(void)
{
    int fd = rb->open(PLSORT_CFG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    int i;
    for (i = 0; i < g_pp_ct; i++)
        rb->fdprintf(fd, "%d %s\n", g_pp[i].mode, g_pp[i].name);
    rb->close(fd);
}
static void pp_load(void)
{
    g_pp_ct = 0;
    int fd = rb->open(PLSORT_CFG, O_RDONLY);
    if (fd < 0) return;
    char line[96];
    while (g_pp_ct < MAX_PLAYLISTS && rb->read_line(fd, line, sizeof(line)) > 0)
    {
        char *sp = rb->strchr(line, ' ');
        if (!sp) continue;
        *sp = '\0';
        int mode = rb->atoi(line);
        if (mode < 0 || mode > SS_ARTIST_ZA) continue;
        rb->strlcpy(g_pp[g_pp_ct].name, sp + 1, sizeof(g_pp[g_pp_ct].name));
        g_pp[g_pp_ct].mode = (unsigned char)mode;
        g_pp_ct++;
    }
    rb->close(fd);
}
static void pp_set_mode(const char *name, int mode)
{
    int i;
    for (i = 0; i < g_pp_ct; i++)
        if (!rb->strcmp(g_pp[i].name, name))
        {
            if (mode < 0) g_pp[i] = g_pp[--g_pp_ct];  /* remove override */
            else g_pp[i].mode = (unsigned char)mode;
            pp_save();
            return;
        }
    if (mode >= 0 && g_pp_ct < MAX_PLAYLISTS)
    {
        rb->strlcpy(g_pp[g_pp_ct].name, name, sizeof(g_pp[g_pp_ct].name));
        g_pp[g_pp_ct].mode = (unsigned char)mode;
        g_pp_ct++;
        pp_save();
    }
}

struct song_ord {
    unsigned long ti;           /* record index within the playlist db */
    unsigned short track_no;
    unsigned short album_rank;  /* album's rank for grouped sorts */
    char key[32];               /* song title, for the flat title sorts */
};
static struct song_ord *g_sord; /* lives in g_decbuf, free during browse */
static int g_sord_n  = 0;       /* entries built; 0 = identity order */
static int g_sord_pl = -1;      /* playlist g_sord was built for */

/* Per-album aggregate used to rank whole albums for the grouped sorts. */
#define ALB_MAX 512
struct alb_ent {
    unsigned long hash;
    unsigned long newest;       /* max date added across the album */
    unsigned short year;
    char name[24];
    char artist[24];
};
static struct alb_ent g_albtab[ALB_MAX];

static int alb_cmp(const void *a, const void *b)
{
    const struct alb_ent *x = a, *y = b;
    int c;
    switch (g_active_sort)
    {
        case SS_ADDED:          /* newest album first */
            if (x->newest != y->newest)
                return (y->newest > x->newest) - (y->newest < x->newest);
            break;
        case SS_YEAR:           /* oldest year first; unknown year last */
        {
            unsigned yx = x->year ? x->year : 0xFFFF;
            unsigned yy = y->year ? y->year : 0xFFFF;
            if (yx != yy) return (yx > yy) - (yx < yy);
            break;
        }
        case SS_ARTIST:
            c = rb->strcasecmp(x->artist, y->artist);
            if (c) return c;
            break;
        case SS_ALBUM_ZA:       /* album name descending */
            c = rb->strcasecmp(x->name, y->name);
            if (c) return -c;
            break;
        case SS_ARTIST_ZA:      /* artist descending, albums ascending */
            c = rb->strcasecmp(x->artist, y->artist);
            if (c) return -c;
            break;
        default:                /* SS_ALBUM: by name ascending */
            break;
    }
    c = rb->strcasecmp(x->name, y->name);   /* tiebreak / album-name sort */
    if (c) return c;
    return (x->hash > y->hash) - (x->hash < y->hash);
}

static int song_cmp(const void *a, const void *b)
{
    const struct song_ord *x = a, *y = b;
    int c;
    if (g_active_sort == SS_TITLE_AZ || g_active_sort == SS_TITLE_ZA)
    {
        c = rb->strcasecmp(x->key, y->key);
        if (c) return (g_active_sort == SS_TITLE_ZA) ? -c : c;
    }
    else    /* grouped: keep each album together, ordered by album rank */
    {
        if (x->album_rank != y->album_rank)
            return (x->album_rank > y->album_rank) -
                   (x->album_rank < y->album_rank);
    }
    /* within an album (or equal titles): track number, then file order */
    if (x->track_no != y->track_no)
        return (x->track_no > y->track_no) - (x->track_no < y->track_no);
    return (x->ti > y->ti) - (x->ti < y->ti);
}

static void song_custom_load(int plidx)
{
    char path[MAX_PATH];
    rb->snprintf(path, sizeof(path), BASE_DIR "/c%08lx.ord",
                 mfnv(g_pls[plidx].name));
    int fd = rb->open(path, O_RDONLY);
    if (fd < 0) return;
    unsigned long cnt = 0;
    int cap = (int)(g_decbuf_sz / sizeof(struct song_ord));
    if (rb->read(fd, &cnt, 4) != 4 || cnt != g_pls[plidx].tracks ||
        cnt > (unsigned long)cap)
    {
        rb->close(fd);
        return;
    }
    unsigned long i;
    for (i = 0; i < cnt; i++)
    {
        unsigned short v;
        if (rb->read(fd, &v, 2) != 2) break;
        g_sord[i].ti = v;
        g_sord[i].track_no = 0;
        g_sord[i].album_rank = 0;
        g_sord[i].key[0] = '\0';
    }
    rb->close(fd);
    g_sord_n = (int)i;
}
static void song_custom_save(int plidx)
{
    if (g_sord_n <= 0) return;
    char path[MAX_PATH];
    rb->snprintf(path, sizeof(path), BASE_DIR "/c%08lx.ord",
                 mfnv(g_pls[plidx].name));
    int fd = rb->open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return;
    unsigned long cnt = (unsigned long)g_sord_n;
    rb->write(fd, &cnt, 4);
    int i;
    for (i = 0; i < g_sord_n; i++)
    {
        unsigned short v = (unsigned short)g_sord[i].ti;
        rb->write(fd, &v, 2);
    }
    rb->close(fd);
}

static void build_song_order(int plidx)
{
    g_sord = (struct song_ord *)g_decbuf;
    g_sord_n = 0;
    g_sord_pl = plidx;

    /* effective mode: per-playlist override if set, else the global default */
    int mode = pp_get_mode(g_pls[plidx].name);
    if (mode < 0)
        mode = g_song_sort;
    g_active_sort = mode;

    if (mode == SS_PLAYLIST)
        return;                             /* identity */

    int cap = (int)(g_decbuf_sz / sizeof(struct song_ord));
    long total = (long)g_pls[plidx].tracks;
    if (total <= 0 || total > cap)
        return;                             /* too big -> identity */

    if (mode == SS_CUSTOM)
    {
        song_custom_load(plidx);
        return;
    }

    bool grouped = (mode != SS_TITLE_AZ && mode != SS_TITLE_ZA);

    char dbpath[MAX_PATH];
    rb->snprintf(dbpath, sizeof(dbpath), DB_DIR "/pl_%03d.plart",
                 (int)g_pls[plidx].dbidx);
    int fd = rb->open(dbpath, O_RDONLY);
    if (fd < 0)
        return;

    struct tr_rec r;
    long i;
    int na = 0;

    /* pass 1 (grouped only): build + rank the album table */
    if (grouped)
    {
        for (i = 0; i < total; i++)
        {
            if (rb->read(fd, &r, sizeof(r)) != (int)sizeof(r))
                break;
            int a;
            for (a = 0; a < na; a++)
                if (g_albtab[a].hash == r.album_hash)
                    break;
            if (a == na && na < ALB_MAX)
            {
                g_albtab[a].hash = r.album_hash;
                g_albtab[a].newest = r.date_added;
                g_albtab[a].year = r.year;
                rb->strlcpy(g_albtab[a].name, r.album,
                            sizeof(g_albtab[a].name));
                rb->strlcpy(g_albtab[a].artist, r.artist,
                            sizeof(g_albtab[a].artist));
                na++;
            }
            else if (a < na)
            {
                if (r.date_added > g_albtab[a].newest)
                    g_albtab[a].newest = r.date_added;
                if (g_albtab[a].year == 0 && r.year)
                    g_albtab[a].year = r.year;
            }
            if ((i & 511) == 0)
                rb->yield();
        }
        if (na > 1)
            rb->qsort(g_albtab, na, sizeof(struct alb_ent), alb_cmp);
    }

    /* pass 2: build the song order array */
    rb->lseek(fd, 0, SEEK_SET);
    for (i = 0; i < total; i++)
    {
        if (rb->read(fd, &r, sizeof(r)) != (int)sizeof(r))
            break;
        struct song_ord *o = &g_sord[g_sord_n];
        o->ti = (unsigned long)i;
        o->track_no = r.track_no;
        o->album_rank = 0;
        o->key[0] = '\0';
        if (grouped)
        {
            int a;
            for (a = 0; a < na; a++)
                if (g_albtab[a].hash == r.album_hash)
                    break;
            o->album_rank = (unsigned short)(a < na ? a : na);
        }
        else
            rb->strlcpy(o->key, r.title, sizeof(o->key));
        g_sord_n++;
        if ((i & 511) == 0)
            rb->yield();
    }
    rb->close(fd);

    if (g_sord_n > 1)
        rb->qsort(g_sord, g_sord_n, sizeof(struct song_ord), song_cmp);
}

static int song_real_index(int plidx, int dispidx)
{
    if (g_sord_n > 0 && g_sord_pl == plidx && dispidx < g_sord_n)
        return (int)g_sord[dispidx].ti;
    return dispidx;
}

static void load_song_sort(void)
{
    unsigned char b = 0;
    int fd = rb->open(SONGSORT_CFG, O_RDONLY);
    if (fd < 0)
        return;
    if (rb->read(fd, &b, 1) == 1 && b <= SS_ARTIST)
        g_song_sort = b;
    rb->close(fd);
}

static void save_song_sort(void)
{
    unsigned char b = (unsigned char)g_song_sort;
    int fd = rb->open(SONGSORT_CFG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return;
    rb->write(fd, &b, 1);
    rb->close(fd);
}

static struct lru_slot *get_thumb(unsigned long hash, bool allow_load)
{
    int i, victim = 0, victim_use = 0x7FFFFFFF;
    for (i = 0; i < LRU_SLOTS; i++)
    {
        if (g_lru[i].album_hash == hash)
        {
            g_lru[i].last_use = ++g_use_counter;
            return &g_lru[i];
        }
        if (g_lru[i].last_use < victim_use)
        {
            victim_use = g_lru[i].last_use;
            victim = i;
        }
    }
    if (!allow_load)
        return NULL;

    char tpath[MAX_PATH];
    thumb_path(hash, tpath, sizeof(tpath));
    int fd = rb->open(tpath, O_RDONLY);
    if (fd < 0)
        return NULL;

    struct lru_slot *s = &g_lru[victim];
    bool ok = rb->read(fd, &s->hdr, sizeof(s->hdr)) == (int)sizeof(s->hdr) &&
              s->hdr.w > 0 && s->hdr.w <= THUMB_SIZE &&
              s->hdr.h > 0 && s->hdr.h <= THUMB_SIZE;
    if (ok)
    {
        int want = (int)((size_t)s->hdr.w * s->hdr.h * sizeof(fb_data));
        ok = rb->read(fd, s->pix, want) == want;
    }
    rb->close(fd);
    if (!ok)
    {
        s->album_hash = 0;
        return NULL;
    }
    s->album_hash = hash;
    s->last_use = ++g_use_counter;
    return s;
}

/* ---------------- drawing ---------------- */

#define COL_BG        LCD_RGBPACK(0, 0, 0)
#define COL_TEXT      LCD_RGBPACK(230, 230, 230)
#define COL_DIM       LCD_RGBPACK(140, 140, 140)
#define COL_SEL_BG    LCD_RGBPACK(40, 90, 160)
#define COL_BAR_BG    LCD_RGBPACK(25, 25, 25)
#define COL_FRAME     LCD_RGBPACK(70, 70, 70)

static void draw_statusbar(const char *title)
{
    char buf[32];
    struct tm *tm = rb->get_time();

    rb->lcd_set_foreground(COL_BAR_BG);
    rb->lcd_fillrect(0, 0, LCD_WIDTH, STATUSBAR_H);
    rb->lcd_set_foreground(COL_TEXT);
    rb->lcd_set_background(COL_BAR_BG);

    rb->snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
    rb->lcd_putsxy(2, 2, buf);

    if (title && title[0])
    {
        char t[48];
        int w, h;
        rb->strlcpy(t, title, sizeof(t));
        fit_string(t, LCD_WIDTH - 110);
        rb->lcd_getstringsize(t, &w, &h);
        rb->lcd_putsxy((LCD_WIDTH - w) / 2, 2, t);
    }

    int x = LCD_WIDTH - 2;

    rb->snprintf(buf, sizeof(buf), "%d%%", rb->battery_level());
    {
        int w, h;
        rb->lcd_getstringsize(buf, &w, &h);
        x -= w;
        rb->lcd_putsxy(x, 2, buf);
    }

    int st = rb->audio_status();
    x -= 12;
    if (st & AUDIO_STATUS_PAUSE)
    {
        rb->lcd_fillrect(x, 4, 3, 8);
        rb->lcd_fillrect(x + 5, 4, 3, 8);
    }
    else if (st & AUDIO_STATUS_PLAY)
    {
        int i;
        for (i = 0; i < 5; i++)
            rb->lcd_vline(x + i, 4 + i, 12 - i);
    }

    if (rb->button_hold())
    {
        x -= 14;
        rb->lcd_putsxy(x, 2, "H");
    }

    rb->lcd_set_background(COL_BG);
}

static void draw_thumb_box(int x, int y, unsigned long hash, bool allow_load)
{
    struct lru_slot *s = get_thumb(hash, allow_load);

    rb->lcd_set_foreground(COL_FRAME);
    rb->lcd_drawrect(x, y, THUMB_SIZE, THUMB_SIZE);

    if (s)
    {
        int ox = x + (THUMB_SIZE - s->hdr.w) / 2;
        int oy = y + (THUMB_SIZE - s->hdr.h) / 2;
        rb->lcd_bitmap_part(s->pix, 0, 0, s->hdr.w,
                            ox, oy, s->hdr.w, s->hdr.h);
    }
    else
    {
        rb->lcd_set_foreground(COL_DIM);
        rb->lcd_putsxy(x + THUMB_SIZE / 2 - 4, y + THUMB_SIZE / 2 - 8, "?");
    }
}

/* ---------------- track screen ---------------- */

static int rows_fit(void)
{
    return (LCD_HEIGHT - STATUSBAR_H - HEADER_H) / ROW_H;
}

static void draw_track_screen(int plidx, int sel, int top, int marquee_px,
                              bool fast, bool moving)
{
    const struct pl_rec *pl = &g_pls[plidx];
    char buf[80];
    int nrows = rows_fit();
    int i;

    rb->lcd_set_foreground(COL_BG);
    rb->lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);

    draw_statusbar(pl->name);

    rb->lcd_set_foreground(COL_DIM);
    if (moving)
        rb->strlcpy(buf, "MOVE: scroll to place, SELECT drops", sizeof(buf));
    else
        rb->snprintf(buf, sizeof(buf), "%d / %lu", sel + 1, pl->tracks);
    rb->lcd_putsxy(4, STATUSBAR_H + 1, buf);
    rb->lcd_set_foreground(COL_FRAME);
    rb->lcd_hline(0, LCD_WIDTH - 1, STATUSBAR_H + HEADER_H - 1);

    for (i = 0; i < nrows; i++)
    {
        int idx = top + i;
        if (idx >= (int)pl->tracks)
            break;

        int y = STATUSBAR_H + HEADER_H + i * ROW_H;
        bool is_sel = (idx == sel);
        const struct tr_rec *tr = get_track(plidx, song_real_index(plidx, idx));

        if (is_sel)
        {
            rb->lcd_set_foreground(COL_SEL_BG);
            rb->lcd_fillrect(0, y, LCD_WIDTH, ROW_H);
        }

        if (!tr)
            continue;

        int tx = 3 + THUMB_SIZE + 6;
        int avail = LCD_WIDTH - tx - 4;

        rb->lcd_set_foreground(COL_TEXT);
        if (is_sel)
            rb->lcd_set_background(COL_SEL_BG);
        {
            char t[64];
            int w, h;
            rb->strlcpy(t, tr->title, sizeof(t));
            rb->lcd_getstringsize(t, &w, &h);
            if (is_sel && w > avail)
            {
                int cycle = w + 40;
                int off = marquee_px % cycle;
                rb->lcd_putsxy(tx - off, y + 5, t);
                if (off > cycle - avail)
                    rb->lcd_putsxy(tx - off + cycle, y + 5, t);
                rb->lcd_set_foreground(COL_SEL_BG);
                rb->lcd_fillrect(0, y, tx, ROW_H);
                rb->lcd_set_foreground(COL_TEXT);
            }
            else
            {
                fit_string(t, avail);
                rb->lcd_putsxy(tx, y + 5, t);
            }
        }

        rb->lcd_set_foreground(COL_DIM);
        {
            char a[52];
            rb->strlcpy(a, tr->artist[0] ? tr->artist : "Unknown artist",
                        sizeof(a));
            fit_string(a, avail);
            rb->lcd_putsxy(tx, y + 21, a);
        }

        if (tr->rating > 0)
        {
            char strating[8];
            int r = MIN(tr->rating, 5), k;
            for (k = 0; k < r; k++)
                strating[k] = '*';
            strating[r] = '\0';
            rb->lcd_set_foreground(COL_TEXT);
            rb->lcd_putsxy(tx, y + 36, strating);
        }

        rb->lcd_set_background(COL_BG);
        draw_thumb_box(3, y + 2, tr->album_hash, !fast);
    }

    rb->lcd_update();
}

static void swap_sord(int a, int b)
{
    struct song_ord t = g_sord[a];
    g_sord[a] = g_sord[b];
    g_sord[b] = t;
}

/* ensure g_sord holds an order we can rearrange (identity if none yet) */
static void ensure_sord_identity(int plidx)
{
    if (g_sord_n > 0)
        return;
    int cap = (int)(g_decbuf_sz / sizeof(struct song_ord));
    long total = (long)g_pls[plidx].tracks;
    if (total > cap) total = cap;
    g_sord = (struct song_ord *)g_decbuf;
    long i;
    for (i = 0; i < total; i++)
    {
        g_sord[i].ti = (unsigned long)i;
        g_sord[i].track_no = 0;
        g_sord[i].album_rank = 0;
        g_sord[i].key[0] = '\0';
    }
    g_sord_n = (int)total;
    g_sord_pl = plidx;
}

/* per-playlist sort menu (opened with MENU from the song view) */
static void pp_sort_menu(int plidx)
{
    MENUITEM_STRINGLIST(m, "Sort this playlist", NULL,
                        "Album A-Z",
                        "Album Z-A",
                        "Artist A-Z",
                        "Artist Z-A",
                        "Recently added",
                        "Year",
                        "Custom (hold SELECT to move)",
                        "Use global default");
    int sel = 0;
    int mode;
    switch (rb->do_menu(&m, &sel, NULL, false))
    {
        case 0: mode = SS_ALBUM;     break;
        case 1: mode = SS_ALBUM_ZA;  break;
        case 2: mode = SS_ARTIST;    break;
        case 3: mode = SS_ARTIST_ZA; break;
        case 4: mode = SS_ADDED;     break;
        case 5: mode = SS_YEAR;      break;
        case 6: mode = SS_CUSTOM;    break;
        case 7: mode = -1;           break;  /* clear -> use global default */
        default: return;                     /* cancelled */
    }
    pp_set_mode(g_pls[plidx].name, mode);
}

static int track_screen(int plidx)
{
    const struct pl_rec *pl = &g_pls[plidx];
    int sel = 0, top = 0;
    build_song_order(plidx);
    int nrows = rows_fit();
    int marquee = 0;
    long last_scroll = *rb->current_tick;
    bool moving = false;
    bool swallow_rel = false;

    while (1)
    {
        bool fast = TIME_BEFORE(*rb->current_tick, last_scroll + HZ / 4);
        if (sel < top)
            top = sel;
        if (sel >= top + nrows)
            top = sel - nrows + 1;

        draw_track_screen(plidx, sel, top, marquee, fast, moving);

        int action = pluginlib_getaction(HZ / 5, plugin_contexts,
                                         ARRAYLEN(plugin_contexts));
        switch (action)
        {
            case PLA_EXIT:
                return -2;

            case PLA_UP:                    /* MENU: sort this playlist */
                if (moving) { moving = false; break; }
                pp_sort_menu(plidx);
                build_song_order(plidx);
                sel = 0; top = 0;
                break;

            case PLA_LEFT:
            case PLA_CANCEL:
                if (moving) { moving = false; break; }
                return -1;

            case PLA_SELECT_REPEAT:         /* hold: pick up song to move */
                if (!moving && pl->tracks > 1)
                {
                    ensure_sord_identity(plidx);
                    moving = true;
                    swallow_rel = true;
                }
                break;

            case PLA_SELECT_REL:
                if (swallow_rel) { swallow_rel = false; break; }
                if (moving)
                {
                    moving = false;
                    pp_set_mode(pl->name, SS_CUSTOM);
                    song_custom_save(plidx);
                    rb->splash(HZ, "Custom order saved");
                    break;
                }
                return sel;                 /* tap: play */

#ifdef HAVE_SCROLLWHEEL
            case PLA_SCROLL_FWD:
            case PLA_SCROLL_FWD_REPEAT:
#endif
            case PLA_DOWN:
            case PLA_DOWN_REPEAT:
                if (moving && g_sord_n > 0)
                {
                    if (sel < g_sord_n - 1) { swap_sord(sel, sel + 1); sel++; }
                }
                else if (sel < (int)pl->tracks - 1)
                    sel++;
                else
                    sel = 0;
                marquee = 0;
                last_scroll = *rb->current_tick;
                break;

#ifdef HAVE_SCROLLWHEEL
            case PLA_SCROLL_BACK:
            case PLA_SCROLL_BACK_REPEAT:
#endif
            case PLA_RIGHT:
                if (moving && g_sord_n > 0)
                {
                    if (sel > 0) { swap_sord(sel, sel - 1); sel--; }
                }
                else if (sel > 0)
                    sel--;
                else
                    sel = (int)pl->tracks - 1;
                marquee = 0;
                last_scroll = *rb->current_tick;
                break;

            default:
                marquee += 10;
                break;
        }
        rb->yield();
    }
}

/* ---------------- playlist screen ---------------- */

static void draw_playlist_screen(int sel, int top, bool moving)
{
    char buf[96];
    int rowh = 24;
    int nrows = (LCD_HEIGHT - STATUSBAR_H) / rowh;
    int i;

    rb->lcd_set_foreground(COL_BG);
    rb->lcd_fillrect(0, 0, LCD_WIDTH, LCD_HEIGHT);
    draw_statusbar(moving ? "Moving - SELECT to drop" : "Playlists");

    if (g_pl_ct == 0)
    {
        rb->lcd_set_foreground(COL_TEXT);
        rb->lcd_putsxy(8, LCD_HEIGHT / 2 - 8, "No playlists found.");
        rb->lcd_putsxy(8, LCD_HEIGHT / 2 + 8, "MENU: update from iTunes");
        rb->lcd_update();
        return;
    }

    for (i = 0; i < nrows; i++)
    {
        int idx = top + i;
        if (idx >= g_pl_ct)
            break;
        int y = STATUSBAR_H + i * rowh;

        if (idx == sel)
        {
            unsigned selcol = moving ? LCD_RGBPACK(190, 110, 20)
                                     : COL_SEL_BG;
            rb->lcd_set_foreground(selcol);
            rb->lcd_fillrect(0, y, LCD_WIDTH, rowh);
            rb->lcd_set_background(selcol);
        }

        rb->lcd_set_foreground(COL_TEXT);
        {
            char t[64];
            rb->strlcpy(t, g_pls[idx].name, sizeof(t));
            fit_string(t, LCD_WIDTH - 70);
            rb->lcd_putsxy(6, y + 4, t);
        }
        rb->lcd_set_foreground(COL_DIM);
        rb->snprintf(buf, sizeof(buf), "%lu", g_pls[idx].tracks);
        {
            int w, h;
            rb->lcd_getstringsize(buf, &w, &h);
            rb->lcd_putsxy(LCD_WIDTH - w - 6, y + 4, buf);
        }
        rb->lcd_set_background(COL_BG);
    }
    rb->lcd_update();
}

static void swap_pls(int a, int b)
{
    struct pl_rec tmp = g_pls[a];
    g_pls[a] = g_pls[b];
    g_pls[b] = tmp;
}

static int playlist_screen(void)
{
    static int sel = 0;
    int top = 0;
    int rowh = 24;
    int nrows = (LCD_HEIGHT - STATUSBAR_H) / rowh;
    bool moving = false;
    bool swallow_rel = false;

    if (sel >= g_pl_ct)
        sel = 0;

    while (1)
    {
        if (sel < top)
            top = sel;
        if (sel >= top + nrows)
            top = sel - nrows + 1;

        draw_playlist_screen(sel, top, moving);

        int action = pluginlib_getaction(HZ / 2, plugin_contexts,
                                         ARRAYLEN(plugin_contexts));
        switch (action)
        {
            case PLA_EXIT:
                return -1;

            case PLA_CANCEL:
                if (moving)
                {
                    moving = false;
                    break;
                }
                return -1;

            case PLA_UP:
                if (moving)
                {
                    moving = false;
                    break;
                }
                return -2;

            case PLA_SELECT_REPEAT:
                /* hold SELECT: pick up the playlist to move it */
                if (!moving && g_pl_ct > 1)
                {
                    moving = true;
                    swallow_rel = true;
                }
                break;

            case PLA_SELECT_REL:
                if (swallow_rel)
                {
                    swallow_rel = false;
                    break;
                }
                if (moving)
                {
                    /* drop: keep this order as the custom order */
                    moving = false;
                    g_sort_mode = PA_SORT_CUSTOM;
                    save_sort();
                    save_custom_order();
                    rb->splash(HZ, "Order saved (sort: Custom)");
                    break;
                }
                if (g_pl_ct > 0)
                    return sel;
                break;

#ifdef HAVE_SCROLLWHEEL
            case PLA_SCROLL_FWD:
            case PLA_SCROLL_FWD_REPEAT:
#endif
            case PLA_DOWN:
            case PLA_DOWN_REPEAT:
                if (g_pl_ct > 0)
                {
                    if (moving)
                    {
                        if (sel < g_pl_ct - 1)
                        {
                            swap_pls(sel, sel + 1);
                            sel++;
                        }
                    }
                    else
                        sel = (sel + 1) % g_pl_ct;
                }
                break;

#ifdef HAVE_SCROLLWHEEL
            case PLA_SCROLL_BACK:
            case PLA_SCROLL_BACK_REPEAT:
#endif
            case PLA_RIGHT:
                if (g_pl_ct > 0)
                {
                    if (moving)
                    {
                        if (sel > 0)
                        {
                            swap_pls(sel, sel - 1);
                            sel--;
                        }
                    }
                    else
                        sel = (sel + g_pl_ct - 1) % g_pl_ct;
                }
                break;

            default:
                break;
        }
        rb->yield();
    }
}

/* ---------------- playback ---------------- */

/* Build the play queue directly from the playlist's db records, in the
 * active sort order, using quiet per-track inserts (no "Inserting tracks"
 * splash, no temp file). CPU-boosted so it stays quick. */
static bool play_from(int plidx, int dispidx)
{
    if (!rb->warn_on_pl_erase())
        return false;

    bool sorted = (g_sord_n > 0 && g_sord_pl == plidx);
    int n = sorted ? g_sord_n : (int)g_pls[plidx].tracks;

    char dbpath[MAX_PATH];
    rb->snprintf(dbpath, sizeof(dbpath), DB_DIR "/pl_%03d.plart",
                 (int)g_pls[plidx].dbidx);
    int fd = rb->open(dbpath, O_RDONLY);
    if (fd < 0)
    {
        rb->splash(HZ * 2, "Could not open playlist data");
        return false;
    }

    if (rb->playlist_create(NULL, NULL) != 0)
    {
        rb->close(fd);
        rb->splash(HZ * 2, "Could not create playlist");
        return false;
    }

    if (n > 300)
        rb->splash(0, "Loading playlist...");
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif

    struct tr_rec r;
    int k, inserted = 0;
    for (k = 0; k < n; k++)
    {
        long ti = sorted ? (long)g_sord[k].ti : (long)k;
        if (read_at(fd, (off_t)ti * sizeof(r), &r, sizeof(r))
            == (int)sizeof(r))
        {
            if (rb->playlist_insert_track(NULL, r.path,
                    PLAYLIST_INSERT_LAST, false, false) < 0)
                break;
            inserted++;
        }
        if ((k & 127) == 0)
            rb->yield();
    }
    rb->playlist_sync(NULL);

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif
    rb->close(fd);

    if (inserted == 0)
    {
        rb->splash(HZ * 2, "Could not load playlist");
        return false;
    }

    /* dispidx is the position within the displayed (sorted) order, which
     * matches the insertion order above */
    rb->playlist_start(dispidx < inserted ? dispidx : 0, 0, 0);
    return true;
}

/* ---------------- options menu ---------------- */

static bool options_menu(void)
{
    MENUITEM_STRINGLIST(menu, "Adam's Playlists", NULL,
                        "Sort playlists",
                        "Sort songs in playlist",
                        "Update from iTunes",
                        "Full rebuild (incl. artwork)",
                        "Help",
                        "Exit plugin");
    MENUITEM_STRINGLIST(sortmenu, "Sort playlists", NULL,
                        "Newest first",
                        "Alphabetical",
                        "iTunes order",
                        "Custom (my order)");
    MENUITEM_STRINGLIST(songmenu, "Sort songs in playlist", NULL,
                        "Playlist order",
                        "Album name",
                        "Date added (by album)",
                        "Album year",
                        "Song title A-Z",
                        "Song title Z-A",
                        "Artist (A-Z)",
                        "Artist (Z-A)");
    int sel = 0;
    switch (rb->do_menu(&menu, &sel, NULL, false))
    {
        case 0:
        {
            int s2 = g_sort_mode;
            int r = rb->do_menu(&sortmenu, &s2, NULL, false);
            if (r >= PA_SORT_NEWEST && r <= PA_SORT_CUSTOM)
            {
                g_sort_mode = r;
                save_sort();
                apply_sort();
            }
            break;
        }
        case 1:
        {
            static const int gmap[] = {
                SS_PLAYLIST, SS_ALBUM, SS_ADDED, SS_YEAR,
                SS_TITLE_AZ, SS_TITLE_ZA, SS_ARTIST, SS_ARTIST_ZA };
            int s3 = 0;
            int r = rb->do_menu(&songmenu, &s3, NULL, false);
            if (r >= 0 && r < (int)ARRAYLEN(gmap))
            {
                g_song_sort = gmap[r];
                save_song_sort();
                g_sord_pl = -1;   /* force rebuild on next playlist open */
            }
            break;
        }
        case 2:
            if (do_sync(false))
            {
                load_index();
                apply_sort();
            }
            break;
        case 3:
            if (do_sync(true))
            {
                load_index();
                apply_sort();
            }
            break;
        case 4:
            rb->splashf(HZ * 4, "Hold SELECT on a playlist to move it. "
                        "MENU opens options. SELECT plays.");
            break;
        case 5:
            return true;
        default:
            break;
    }
    return false;
}

/* ---------------- entry point ---------------- */

enum plugin_status plugin_start(const void *parameter)
{
    (void)parameter;
    int i;

    g_buf = rb->plugin_get_buffer(&g_bufsz);
    if (!g_buf || g_bufsz < 1024 * 1024)
    {
        rb->splash(HZ * 2, "Not enough memory");
        return PLUGIN_ERROR;
    }

    unsigned char *p = g_buf;

    p = (unsigned char *)ALIGN_UP((uintptr_t)p, 8);

#define CARVE(ptr, type, count) \
    do { ptr = (type *)p; p += sizeof(type) * (count); \
         p = (unsigned char *)ALIGN_UP((uintptr_t)p, 8); } while (0)

    CARVE(g_idmap, struct idmap_ent, MAX_LIB_TRACKS);
    CARVE(g_albums, struct album_ent, MAX_ALBUMS);
    CARVE(g_artmap, struct art_ent, MAX_ARTMAP);
    CARVE(g_pls, struct pl_rec, MAX_PLAYLISTS);
    CARVE(g_lru_pix, fb_data, (size_t)LRU_SLOTS * THUMB_SIZE * THUMB_SIZE);

    if ((size_t)(p - g_buf) + 256 * 1024 > g_bufsz)
    {
        rb->splash(HZ * 2, "Not enough memory");
        return PLUGIN_ERROR;
    }
    g_decbuf = p;
    g_decbuf_sz = g_bufsz - (size_t)(p - g_buf);

    for (i = 0; i < LRU_SLOTS; i++)
    {
        g_lru[i].album_hash = 0;
        g_lru[i].last_use = 0;
        g_lru[i].pix = g_lru_pix + (size_t)i * THUMB_SIZE * THUMB_SIZE;
    }
    for (i = 0; i < RECWIN_SLOTS; i++)
        g_recwin[i].index = -1;

    rb->lcd_setfont(FONT_UI);
#if LCD_DEPTH > 1
    rb->lcd_set_backdrop(NULL);
#endif
    rb->lcd_set_background(COL_BG);
    rb->lcd_set_foreground(COL_TEXT);

    if (sync_needed())
    {
        if (!rb->file_exists(ITUNESDB_PATH))
        {
            rb->splashf(HZ * 3, "No iTunesDB found. Sync this iPod "
                                "with iTunes first.");
            if (!rb->file_exists(IDX_FILE))
                return PLUGIN_OK;
        }
        else
            do_sync(false);
    }

    if (!load_index())
    {
        rb->splash(HZ * 2, "No playlist data. Run update from the menu.");
        g_pl_ct = 0;
    }
    load_sort();
    load_song_sort();
    pp_load();
    apply_sort();

    while (1)
    {
        int pl = playlist_screen();
        if (pl == -1)
            break;
        if (pl == -2)
        {
            if (options_menu())
                break;
            continue;
        }

        while (1)
        {
            int tr = track_screen(pl);
            if (tr == -1)
                break;
            if (tr == -2)
                return PLUGIN_OK;
            if (tr >= 0 && play_from(pl, tr))
                return PLUGIN_GOTO_WPS;
        }
    }

    return PLUGIN_OK;
}
