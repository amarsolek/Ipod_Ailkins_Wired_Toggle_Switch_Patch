/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 by Hemant Kumar
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

/* "Mikey" is the internal controller for the headphone jack microphone and
 * the inline earphone remote (I2C bus 0, address 0x72).
 *
 * Register protocol (reverse engineered on-device, 2026-07):
 *   reg0 = 0x2f     remote-reporting mode. The low 3 bits are the mic bias
 *                   level (7, same as the recording path uses); 0x28 arms
 *                   the button detection engine. The chip resets on jack
 *                   removal (all registers read zero afterwards), so the
 *                   mode is re-armed on insertion.
 *   reg4 & 0x01     center (play/pause) held. Bit 2 (0x04) and the high
 *                   base bit vary between remote units and are NOT the
 *                   button (verified by on-device register capture), so
 *                   only bit 0 is tested. Can flicker while the mode is
 *                   (re)armed, hence the "armed" guard below.
 *   reg5            volume edge events, each readable for ~100ms:
 *                   0x04 vol+ press, 0x08 vol+ release,
 *                   0x01 vol- press, 0x02 vol- release.
 *                   0x30 is the accessory-ID event (ignored).
 * The remote itself signals volume as static DC loads on the mic line and
 * identifies via a one-time ultrasonic chirp (the same scheme David Carne
 * documented for the shuffle 3G remote); Mikey does that level and chirp
 * detection in hardware and reports the results as above. */

#include "system.h"
#include "cpu.h"
#include "kernel.h"
#include "thread.h"
#include "button.h"
#include "i2c-s5l8702.h"
#include "mikey-target.h"

#define MIKEY_ADDR      0x72
#define MIKEY_REG_MODE  0       /* mic bias + detection-engine mode */
#define MIKEY_REG_BTN   4       /* center-button level */
#define MIKEY_REG_EVT   5       /* volume press/release edge events */

#define MIKEY_MODE_OFF      0x05    /* reset default, mic line unpowered */
#define MIKEY_MODE_MIC      0x07    /* mic bias raised, for recording */
#define MIKEY_MODE_REMOTE   0x2f    /* mic bias + button detection */

#define MIKEY_BTN_CENTER    0x01    /* bit0 = center; bit2 is base level on this remote */
#define MIKEY_EVT_VOLUP_DN  0x04
#define MIKEY_EVT_VOLUP_UP  0x08
#define MIKEY_EVT_VOLDN_DN  0x01
#define MIKEY_EVT_VOLDN_UP  0x02


/* ---- level-style remotes -------------------------------------------------
 * Not every inline remote speaks Apple's scheme. Captured on this device
 * from a third-party remote (2026-10-04, 32 samples):
 *
 *   idle            r4=0x20            (bit5 is a constant base level)
 *   button down     r4=0x24            (bit2 = "a button is down")
 *   which button    r5=0x04 vol+, r5=0x01 vol-, r5=0x00 centre
 *
 * r4 bit0 - Apple's centre bit - is never set, and r5 NEVER carries a
 * release event (0x08 / 0x02). Two consequences:
 *
 *  - A centre mask of 0x05 matches bit2 and so fires on EVERY button, which
 *    reads as the player pausing whenever you touch volume. A mask of 0x01
 *    never matches at all, so centre does nothing. Both are wrong for this
 *    remote; neither is a mask problem, it is a decoding-model problem.
 *  - mikey_decode_vol() latches held=true on a press event and waits for a
 *    release event that never arrives, so volume runs to the rail.
 *
 * So for these remotes decode the bit2 LEVEL instead, latching which button
 * from r5 at the rising edge, and report a fixed-length click. A level
 * cannot run away, and a click cannot stick.
 *
 * One physical press bounces: rise+event, fall, then a second eventless rise
 * ~200ms later. An eventless rise is therefore only treated as a centre press
 * when no other press has been accepted recently; that collapses the bounce
 * without blocking real centre presses. */
#define MIKEY_LVL_DOWN        0x04   /* r4: a button is down */
#define MIKEY_LVL_PULSE_POLLS 3      /* reported click length, 60ms */
#define MIKEY_LVL_OFF_POLLS   2      /* release debounce, 40ms */
#define MIKEY_LVL_HOLD_CAP    100    /* 2s: last-resort stop for a stuck hold */
#define MIKEY_LVL_REFRACTORY  30     /* 600ms: swallow the post-press bounce */

/* Close the decision window: an r5 press event names the button, no event
 * means the centre button. Centre is additionally gated on the refractory
 * period, because one physical press bounces into a second, eventless rise
 * a few hundred ms later and that must not read as a centre click. */
static void lvl_decide(unsigned char evt, bool armed, int *pending,
                       int *btn, int *pulse, int *refract)
{
    int want = 0;
    *pending = 0;
    if (evt & MIKEY_EVT_VOLUP_DN)
        want = BUTTON_RC_VOL_UP;
    else if (evt & MIKEY_EVT_VOLDN_DN)
        want = BUTTON_RC_VOL_DOWN;
    else if (*refract == 0)
        want = BUTTON_MULTIMEDIA_PLAYPAUSE;

    if (want && armed)
    {
        *btn = want;
        *pulse = MIKEY_LVL_PULSE_POLLS;
        *refract = MIKEY_LVL_REFRACTORY;
    }
}

/* A press+release pair landing in a single poll is stretched over this
 * many polls, so the button driver's debounce (two identical consecutive
 * reads at the 10ms button tick) still accepts the click. */
#define MIKEY_CLICK_POLLS   2

/* Center-button handling: click-only. reg4's rising edge is always
 * immediate, but after ~5s of button inactivity the chip's event engine
 * naps and reports the release (reg4 fall, reg6 events) ~1.8s late, so
 * the press duration is unreliable. Every press is therefore reported
 * as a fixed-length click at the rise; there is no hold/long-press. A
 * release must be seen for several consecutive polls before a new rise
 * counts, so bounce or an I2C NAK can't double-fire a click. */
#define MIKEY_CENTER_PULSE_POLLS  3     /* reported click length, 60ms */
#define MIKEY_CENTER_OFF_POLLS    3     /* release debounce, 60ms */

unsigned char mikey_read(int address)
{
    /* default to "no press, no events" if the transfer fails: the chip
     * NAKs while it resets itself around jack removal/insertion */
    unsigned char val = 0;
    i2c_read(0, MIKEY_ADDR, address, 1, &val);
    return val;
}

int mikey_write(int address, unsigned char val)
{
    return i2c_write(0, MIKEY_ADDR, address, 1, &val);
}

void mikey_reset(void)
{
    mikey_write(MIKEY_REG_MODE, MIKEY_MODE_OFF);
    mikey_write(1, 0x80);
}

static int mikey_btn = BUTTON_NONE;
static bool mikey_seen = false;
static volatile bool mic_active = false;
static long mikey_stack[DEFAULT_STACK_SIZE/2/sizeof(long)];

/* The recording path owns the chip while the jack mic is enabled; the
 * polling thread backs off and re-arms the remote mode afterwards. */
void mikey_set_mic_capture(bool enable)
{
    mic_active = enable;
    if (enable)
        mikey_write(MIKEY_REG_MODE, MIKEY_MODE_MIC);
    else
        mikey_reset();
}

/* Decode one volume button's press/release edge events into a held state.
 * A real hold only produces the two edges, so between them *held carries
 * the state; *click stretches a same-poll press+release (see above). */

/* ---- diagnostic capture -------------------------------------------------
 * The debug screen's own loop exits when a button is released, which makes
 * it useless for watching a remote while you press it. So the capture lives
 * here, in the polling thread: press buttons with the screen closed, then
 * open it and read what was recorded.
 *
 * reg5 is an edge-event register readable for only ~100ms, so a live
 * display would miss almost every press. We keep an OR-latch of every bit
 * ever seen plus a short de-duplicated history of distinct (reg4, reg5)
 * observations, which is enough to recover what a given remote actually
 * signals for each button. */
#define MIKEY_DBG_SLOTS 10
static unsigned char dbg_r4[MIKEY_DBG_SLOTS];
static unsigned char dbg_r5[MIKEY_DBG_SLOTS];
static unsigned short dbg_count;        /* total distinct observations */
static unsigned char dbg_r4_or, dbg_r5_or;
static unsigned char dbg_last_r4, dbg_last_r5;

static void mikey_dbg_record(unsigned char r4, unsigned char r5)
{
    dbg_r4_or |= r4;
    dbg_r5_or |= r5;
    if (!r4 && !r5)
        return;                          /* idle: nothing to record */
    if (r4 == dbg_last_r4 && r5 == dbg_last_r5)
        return;                          /* unchanged: collapse repeats */
    dbg_last_r4 = r4;
    dbg_last_r5 = r5;
    dbg_r4[dbg_count % MIKEY_DBG_SLOTS] = r4;
    dbg_r5[dbg_count % MIKEY_DBG_SLOTS] = r5;
    dbg_count++;
}

/* Oldest-first copy of the retained history. Returns how many slots filled. */
int mikey_debug_history(unsigned char *r4out, unsigned char *r5out, int max)
{
    int have = (dbg_count < MIKEY_DBG_SLOTS) ? dbg_count : MIKEY_DBG_SLOTS;
    int start = (dbg_count < MIKEY_DBG_SLOTS) ? 0 : (dbg_count % MIKEY_DBG_SLOTS);
    int n = (have < max) ? have : max;
    for (int i = 0; i < n; i++)
    {
        int idx = (start + i) % MIKEY_DBG_SLOTS;
        r4out[i] = dbg_r4[idx];
        r5out[i] = dbg_r5[idx];
    }
    return n;
}

void mikey_debug_summary(unsigned char *r4_or, unsigned char *r5_or,
                         unsigned short *count)
{
    *r4_or = dbg_r4_or;
    *r5_or = dbg_r5_or;
    *count = dbg_count;
}

void mikey_debug_clear(void)
{
    dbg_count = 0;
    dbg_r4_or = dbg_r5_or = 0;
    dbg_last_r4 = dbg_last_r5 = 0;
}

static void mikey_decode_vol(unsigned char evt, unsigned char dn,
                             unsigned char up, bool *held, int *click)
{
    if (*click > 0 && --*click == 0)
        *held = false;

    if (evt & dn)
    {
        *held = true;
        *click = (evt & up) ? MIKEY_CLICK_POLLS : 0;
    }
    else if ((evt & up) && *click == 0)
        *held = false;
}

static void mikey_thread(void)
{
    bool powered = false;
    bool armed = false;   /* need a released reading before first press */
    bool vol_up = false, vol_dn = false;
    int up_click = 0, dn_click = 0;
    bool center_down = false;   /* debounced "press already reported" */
    int center_off = MIKEY_CENTER_OFF_POLLS;   /* consecutive released */
    int center_pulse = 0;       /* click pulse countdown */

    /* level-style remote state (see MIKEY_LVL_* above) */
    bool lvl_down = false;      /* debounced bit2 level */
    int  lvl_off = MIKEY_LVL_OFF_POLLS;
    int  lvl_pulse = 0;
    int  lvl_btn = 0;           /* which button the pulse reports */
    int  lvl_pending = 0;       /* decision window countdown */
    int  lvl_refract = 0;

    while (1)
    {
        if (!headphones_inserted() || mic_active)
        {
            if (powered && !mic_active)
                mikey_reset();
            powered = false;
            armed = false;
            vol_up = vol_dn = false;
            up_click = dn_click = 0;
            center_down = false;
            center_off = MIKEY_CENTER_OFF_POLLS;
            center_pulse = 0;
            lvl_down = false; lvl_off = MIKEY_LVL_OFF_POLLS;
            lvl_pulse = lvl_btn = lvl_pending = lvl_refract = 0;
            mikey_btn = BUTTON_NONE;
            sleep(HZ/2);   /* nothing to poll, check back at leisure */
            continue;
        }
        powered = true;
        mikey_seen = true;

        /* Re-arm when the mode readback disagrees (first pass, chip reset
         * on jack removal, or the recording path rewrote it). Arming can
         * flicker reg4 and emits a spurious accessory-ID event, so drop
         * the state and settle for one poll. */
        if (mikey_read(MIKEY_REG_MODE) != MIKEY_MODE_REMOTE)
        {
            mikey_write(MIKEY_REG_MODE, MIKEY_MODE_REMOTE);
            armed = false;
            vol_up = vol_dn = false;
            up_click = dn_click = 0;
            center_down = false;
            center_off = MIKEY_CENTER_OFF_POLLS;
            center_pulse = 0;
            lvl_down = false; lvl_off = MIKEY_LVL_OFF_POLLS;
            lvl_pulse = lvl_btn = lvl_pending = lvl_refract = 0;
            mikey_btn = BUTTON_NONE;
            sleep(HZ/50);
            continue;
        }

        unsigned char evt = mikey_read(MIKEY_REG_EVT);
        unsigned char r4  = mikey_read(MIKEY_REG_BTN);
        mikey_dbg_record(r4, evt);


        /* One decoder for both remote styles. Measured on this device:
         *
         *   idle              r4 = 0x20          (bit5 constant base level)
         *   volume button     r4 = 0x24          (bit2 = a button is down)
         *   centre button     r4 = 0x25          (bit2 + bit0)
         *   which volume      r5 = 0x04 up, 0x01 down
         *
         * Both Apple and third-party remotes use r4 bit0 for centre, so
         * there is nothing to auto-detect - an earlier version latched
         * "this is an Apple remote" on bit0 and so disabled volume for the
         * rest of the session the first time centre was pressed.
         *
         * They differ only in how a volume hold ENDS. Apple sends a release
         * event in r5 (0x08 / 0x02); this remote sends none at all, which is
         * why waiting for one let volume run to the rail. So end a hold on
         * either signal: a release event, or r4 bit2 going clear. A level
         * cannot get stuck, and the poll cap below is a last-resort stop. */
        bool any_down = (r4 & MIKEY_LVL_DOWN) != 0;

        if (evt & MIKEY_EVT_VOLUP_DN) { vol_up = true;  up_click = MIKEY_LVL_HOLD_CAP; }
        if (evt & MIKEY_EVT_VOLDN_DN) { vol_dn = true;  dn_click = MIKEY_LVL_HOLD_CAP; }

        if ((evt & MIKEY_EVT_VOLUP_UP) || !any_down || (up_click && !--up_click))
            vol_up = false;
        if ((evt & MIKEY_EVT_VOLDN_UP) || !any_down || (dn_click && !--dn_click))
            vol_dn = false;

        /* Centre: bit0, click-only. reg4 can read pressed while the mode
         * settles, so it must be seen released once before any press counts. */
        bool raw = (r4 & MIKEY_BTN_CENTER) != 0;
        if (!raw)
            armed = true;

        if (raw && armed)
        {
            if (!center_down && center_off >= MIKEY_CENTER_OFF_POLLS)
                center_pulse = MIKEY_CENTER_PULSE_POLLS;
            center_down = true;
            center_off = 0;
        }
        else
        {
            if (center_off < MIKEY_CENTER_OFF_POLLS)
                center_off++;
            if (center_off >= MIKEY_CENTER_OFF_POLLS)
                center_down = false;
        }

        bool center = false;
        if (center_pulse > 0)
        {
            center = true;
            center_pulse--;
        }

        mikey_btn = (center ? BUTTON_MULTIMEDIA_PLAYPAUSE : 0)
                  | (vol_up ? BUTTON_RC_VOL_UP   : 0)
                  | (vol_dn ? BUTTON_RC_VOL_DOWN : 0);

        /* volume edge events stay readable for ~100ms; 20ms leaves a
         * comfortable margin against scheduling jitter */
        sleep(HZ/50);
    }
}

/* Upstream's hardware debug screen (debug-s5l8702.c) calls this.
 * This driver does not gate anything on the ID-chirp latch, so
 * report presence as 'the chip ACKed the bus at least once'. */
bool mikey_present(void)
{
    return mikey_seen;
}

int mikey_button_read(void)
{
    return mikey_btn;
}

void mikey_init(void)
{
    mikey_reset();
    create_thread(mikey_thread, mikey_stack, sizeof(mikey_stack), 0,
                  "mikey" IF_PRIO(, PRIORITY_SYSTEM) IF_COP(, CPU));
}
