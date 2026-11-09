/**
 * Journey Bag automation (see journeybag.h).
 *
 * The menu state it reads (found by memory dumps of a walk through both
 * paths, 2026-11-08):
 *
 *   [0xAE73F0]  main mode; & 0x7F == 1 title and its menus, 0x1D Prepare
 *               for Battle; bit 0x80 = initialised
 *   title struct 0x5EB620:
 *     +0x00     0 "Press START", 2 the menu is idle
 *     +0x10     cursor: 0 Story Mode, 1 Vs. Mode, 6 Vs. Play, 7 Prepare
 *               for Battle (the game sets mode 0x1D on A, sub_000133E0)
 *   Prepare for Battle:
 *     [0x658098] byte 1 = menu cursor (0 Create, 1 Return)
 *                byte 2 = 0 the menu, 1 the Create screens, 2 Return
 *     [0x65809C] byte 0 = screen within it:
 *                Create: 1 home-save list, 3 memory-unit list,
 *                        5 "Your Journey Bag has been saved."
 *                        8 "Is it OK to overwrite this?" (a bag is
 *                          already on the unit)
 *                Return: 1 memory-unit list, 7 "Bringing it back will
 *                        delete the Journey Bag, proceed?" (cursor on NO),
 *                        0x0A "restored" + "erased",
 *                        2 "Your Journey Bag could not be found"
 *     [0x6580DD] home-save list cursor (0 = slot 1)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#include "journeybag.h"

extern ptrdiff_t g_xbox_mem_offset;
int input_rec_mode(void);            /* recomp_manual.c: 2 = replaying */

#define M8(va)  (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M32(va) (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

#define G_MODE      0x00AE73F0u
#define TITLE       0x005EB620u
#define PFB_MENU    0x00658098u
#define PFB_SCREEN  0x0065809Cu
#define PFB_SAVECUR 0x006580DDu

/* XINPUT_STATE: +0 packet, +4 digital buttons, +6 analog A, B, X, Y... */
enum { K_NONE, K_UP, K_DOWN, K_START, K_A, K_B };

#define HOLD_POLLS   4                /* a press: held this many polls */
#define GAP_POLLS    8                /* then released this many */
#define LIMIT_POLLS  (60 * 180)       /* about three minutes of polls */

static int s_job = -1;                /* -1 unread, 0 none, 1 create, 2 return */
static int s_slot;                    /* create: home-save slot, 0-based */
static int s_overwrite;               /* create: may replace a bag on the unit */
static int s_exit;
static int s_finished;
static uint32_t s_handle;             /* the first controller polled */
static int s_key, s_hold, s_gap;
static uint32_t s_polls;
static int s_seen_done;               /* the success screen was reached */
static int s_confirm_tries;

static void finish(int ok, const char *what)
{
    s_finished = 1;
    fprintf(stderr, "[JBAG] %s: %s\n", ok ? "done" : "failed", what);
    fflush(stderr);
    if (s_exit)
        exit(ok ? 0 : 5);
}

static void read_job(void)
{
    const char *e = getenv("RECOMP_JOURNEYBAG");
    s_job = 0;
    if (!e || !*e)
        return;
    if (!strncmp(e, "create:", 7) && (e[7] == '1' || e[7] == '2')
            && (!e[8] || !strcmp(e + 8, ":overwrite"))) {
        s_job = 1;
        s_slot = e[7] - '1';
        s_overwrite = e[8] != 0;
    } else if (!strcmp(e, "return")) {
        s_job = 2;
    } else {
        fprintf(stderr, "[JBAG] failed: RECOMP_JOURNEYBAG=%s (want create:1, "
                "create:2, create:<slot>:overwrite or return)\n", e);
        s_finished = 1;
        return;
    }
    s_exit = getenv("RECOMP_JOURNEYBAG_EXIT") != NULL;
    fprintf(stderr, "[JBAG] %s%s: driving Vs. Mode > Prepare for Battle\n",
            s_job == 1 ? "create from home save " : "return to the home save",
            s_job == 1 ? (s_slot ? "2" : "1") : "");
}

/* The next button for the state the menus are in, or K_NONE to wait. */
static int decide(void)
{
    uint32_t m = M32(G_MODE), mode = m & 0x7Fu;

    if (mode == 1 && (m & 0x80u)) {
        uint32_t ts = M32(TITLE), cur = M32(TITLE + 0x10);
        if (ts == 0)
            return K_START;
        if (ts != 2)
            return K_NONE;            /* a transition */
        switch (cur) {
        case 0: return K_DOWN;        /* Story Mode -> Vs. Mode */
        case 1: return K_A;           /* open Vs. Mode */
        case 6: return K_DOWN;        /* Vs. Play -> Prepare for Battle */
        case 7: return K_A;
        default: return K_B;          /* some other submenu: back out */
        }
    }
    if (mode == 0x1D && (m & 0x80u)) {
        uint32_t menu = M32(PFB_MENU), scr = M32(PFB_SCREEN) & 0xFFu;
        uint32_t flow = (menu >> 16) & 0xFFu, cur = (menu >> 8) & 0xFFu;
        int want = s_job == 1 ? 0 : 1;
        if (flow == 0)
            return (int)cur != want ? K_DOWN : K_A;
        if ((int)flow != s_job)
            return K_B;               /* the other one opened: back out */
        if (s_job == 1) {
            switch (scr) {
            case 1:                   /* home-save list */
                if (M8(PFB_SAVECUR) != (uint8_t)s_slot)
                    return M8(PFB_SAVECUR) < (uint8_t)s_slot ? K_DOWN : K_UP;
                return K_A;
            case 3:                   /* memory-unit list: MU 1A is first */
                if (s_seen_done) {
                    finish(1, s_slot ? "Journey Bag created from home save 2 on MU 1A"
                                     : "Journey Bag created from home save 1 on MU 1A");
                    return K_NONE;
                }
                return K_A;
            case 5:                   /* "Your Journey Bag has been saved." */
                s_seen_done = 1;
                return K_A;
            case 8:                   /* "Is it OK to overwrite this?" */
                /* A bag is already on the unit. Overwriting it orphans
                 * that bag: its home save keeps the token and can never
                 * get its progress back. Only with ":overwrite"; then as
                 * on the Return dialog, up and A until the save goes. */
                if (!s_overwrite) {
                    finish(0, "a Journey Bag is already on MU 1A (return it first, "
                              "or create:<slot>:overwrite)");
                    return K_NONE;
                }
                if (s_confirm_tries++ & 1)
                    return K_A;
                return K_UP;
            default:
                return K_NONE;
            }
        }
        switch (scr) {                /* Return */
        case 1:                       /* memory-unit list */
            if (s_seen_done) {
                finish(1, "Journey Bag returned to its home save and erased from MU 1A");
                return K_NONE;
            }
            return K_A;
        case 7:                       /* "...delete the Journey Bag, proceed?" */
            /* The cursor starts on NO; up selects YES. If the game takes
             * the A as NO it goes back to the list and this comes round
             * again with the cursor moved (two tries). */
            if (s_confirm_tries++ & 1)
                return K_A;
            return K_UP;
        case 0x0A:                    /* "restored" + "erased" */
            s_seen_done = 1;
            return K_A;
        case 2:
            finish(0, "no Journey Bag on MU 1A (\"could not be found\")");
            return K_NONE;
        default:
            return K_NONE;
        }
    }
    return K_NONE;                    /* loading or another mode: wait */
}

static void press(uint8_t *st, int key)
{
    switch (key) {
    case K_UP:    st[4] |= 0x01; break;
    case K_DOWN:  st[4] |= 0x02; break;
    case K_START: st[4] |= 0x10; break;
    case K_A:     st[6] = 0xFF;  break;
    case K_B:     st[7] = 0xFF;  break;
    default: break;
    }
    (*(uint32_t *)st)++;              /* a new packet: the state changed */
}

void journeybag_pre_poll(uint32_t handle, uint8_t *st)
{
    if (s_job < 0)
        read_job();
    if (!s_job || s_finished || !st || input_rec_mode() == 2)
        return;
    if (!s_handle)
        s_handle = handle;
    if (handle != s_handle)
        return;                       /* the first controller only */
    if (++s_polls > LIMIT_POLLS) {
        char why[96];
        snprintf(why, sizeof why, "no progress after %u polls (mode 0x%X, "
                 "menu 0x%X, screen 0x%X)", s_polls - 1, M32(G_MODE),
                 M32(PFB_MENU), M32(PFB_SCREEN));
        finish(0, why);
        return;
    }
    if (s_hold) {                     /* the press goes on */
        s_hold--;
        press(st, s_key);
        if (!s_hold)
            s_gap = GAP_POLLS;
        return;
    }
    if (s_gap) {                      /* released, the menu reacts */
        s_gap--;
        return;
    }
    s_key = decide();
    if (s_key != K_NONE && !s_finished) {
        s_hold = HOLD_POLLS - 1;
        press(st, s_key);
    }
}
