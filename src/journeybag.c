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
#include <time.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <io.h>                       /* _findfirst */
#define PATH_SEP "\\"
#else
#include <dirent.h>
#define PATH_SEP "/"
#ifndef __stdcall
#define __stdcall
#endif
#endif

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

/* -1 unread, 0 none, 1 create, 2 return, 3 verify */
enum { JOB_NONE, JOB_CREATE, JOB_RETURN, JOB_VERIFY };
static int s_job = -1;
static int s_slot;                    /* create/verify: home-save slot, 0-based */
static int s_verify_settle;           /* verify: polls on the save list so far */
static int s_verify_result = -1;      /* verify: -1 not yet, 0 bad, 1 good */
static char s_verify_why[200];
static int s_overwrite;               /* create: may replace a bag on the unit */
static int s_exit;
static int s_finished;
static uint32_t s_handle;             /* the first controller polled */
static int s_key, s_hold, s_gap;
static uint32_t s_polls;
static int s_seen_done;               /* the success screen was reached */
static int s_confirm_tries;
static int s_alien_polls;             /* decisions made in an unexpected mode */
static int s_pressed_start;           /* presses of Start on the title */
static int s_seen_menu;               /* the title menu has been open */
static uint32_t s_wait_state[4];      /* the menu state while waiting */
static int s_last_key, s_same_presses; /* repeated presses on one state */
static uint32_t s_last_state[5];
static int s_wait_polls;              /* polls of waiting on that state */

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
    s_exit = getenv("RECOMP_JOURNEYBAG_EXIT") != NULL;   /* before any failure */
    if (!strncmp(e, "create:", 7) && (e[7] == '1' || e[7] == '2')
            && (!e[8] || !strcmp(e + 8, ":overwrite"))) {
        s_job = 1;
        s_slot = e[7] - '1';
        s_overwrite = e[8] != 0;
    } else if (!strcmp(e, "return")) {
        s_job = JOB_RETURN;
    } else if (!strncmp(e, "verify:", 7) && (e[7] == '1' || e[7] == '2') && !e[8]) {
        s_job = JOB_VERIFY;
        s_slot = e[7] - '1';
    } else {
        char why[200];
        snprintf(why, sizeof why, "RECOMP_JOURNEYBAG=%.60s (want create:1, create:2, "
                 "create:<slot>:overwrite, return or verify:<slot>)", e);
        finish(0, why);               /* exits 5 with RECOMP_JOURNEYBAG_EXIT */
        return;
    }
    fprintf(stderr, "[JBAG] %s%s: driving Vs. Mode > Prepare for Battle\n",
            s_job == JOB_CREATE ? "create from home save " :
            s_job == JOB_VERIFY ? "verify home save " : "return to the home save",
            s_job == JOB_RETURN ? "" : (s_slot ? "2" : "1"));
}

/* ── Checks on the save files (verify, the return summary) ──
 *
 * blinx2data.bin is 0x14000 bytes: two 0x84C8-byte records, each its
 * 0x84B4 bytes of data then a 20-byte signature (XCalculateSignature, the
 * title's roamable key; in this runtime XboxSignatureKey is 16 zero bytes,
 * xboxrecomp kernel_bridge.c, so the signature is HMAC-SHA1 with that
 * key). An unused second record has an all-zero signature. The record's
 * layout is the in-memory one cheats.c uses: team name +0x2C, team blocks
 * +0x65C0 (Time Sweepers) and +0x746C (Tom-Tom Gang), gold at block +0x14,
 * item state bytes at block +0x74/+0xB4/+0xF4/+0x114. */
#define REC_SIZE   0x84C8u
#define REC_DATA   0x84B4u
#define FILE_SIZE  0x14000u
#define TITLE_DIR  "UDATA" PATH_SEP "4d530065"

void __stdcall xbox_XcHMAC(const unsigned char *key, unsigned long key_len,
                           const unsigned char *d1, unsigned long d1_len,
                           const unsigned char *d2, unsigned long d2_len,
                           unsigned char *digest);

static int record_signed(const uint8_t *rec, int may_be_unused)
{
    static const uint8_t key[16];
    uint8_t want[20];
    static const uint8_t zero[20];
    if (may_be_unused && !memcmp(rec + REC_DATA, zero, 20))
        return 1;
    xbox_XcHMAC(key, 16, rec, REC_DATA, NULL, 0, want);
    return !memcmp(want, rec + REC_DATA, 20);
}

static const char *save_root(void)
{
    const char *d = getenv("RECOMP_SAVE_DIR");
    return d && *d ? d : getenv("RECOMP_GAME_DIR");
}

/* The home-save folder names under <save dir>/UDATA/<title>, up to max;
 * returns how many. One portable directory walk for every caller. */
#define MAX_SAVES 64
static int save_folders(char names[][64], int max)
{
    char dir[600];
    const char *root = save_root();
    int n = 0;
    if (!root)
        return 0;
    snprintf(dir, sizeof dir, "%s" PATH_SEP TITLE_DIR, root);
#if defined(_WIN32)
    {
        char pat[620];
        struct _finddata_t fd;
        intptr_t h;
        snprintf(pat, sizeof pat, "%s\\*", dir);
        if ((h = _findfirst(pat, &fd)) == -1)
            return 0;
        do {
            if ((fd.attrib & _A_SUBDIR) && fd.name[0] != '.' && n < max)
                snprintf(names[n++], 64, "%s", fd.name);
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (!d)
            return 0;
        while ((e = readdir(d)) != NULL && n < max) {
            char path[700];
            struct stat st;
            if (e->d_name[0] == '.')
                continue;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
                snprintf(names[n++], 64, "%s", e->d_name);
        }
        closedir(d);
    }
#endif
    return n;
}

static void data_path(char *path, size_t len, const char *folder)
{
    snprintf(path, len, "%s" PATH_SEP TITLE_DIR PATH_SEP "%s" PATH_SEP "blinx2data.bin",
             save_root(), folder);
}

/* Reads every home save's blinx2data.bin under <save dir>/UDATA/<title>.
 * pick(): 1 to stop at this one. Returns 1 if one was picked; its folder
 * name and contents are left in name/buf. */
static int each_save(int (*pick)(const uint8_t *buf, void *ctx), void *ctx,
                     uint8_t *buf, char *name, size_t name_len)
{
    static char names[MAX_SAVES][64];
    char path[700];
    int n = save_folders(names, MAX_SAVES), i;
    for (i = 0; i < n; i++) {
        FILE *f;
        size_t got;
        data_path(path, sizeof path, names[i]);
        if (!(f = fopen(path, "rb")))
            continue;
        got = fread(buf, 1, FILE_SIZE, f);
        fclose(f);
        if (got == FILE_SIZE && pick(buf, ctx)) {
            snprintf(name, name_len, "%s", names[i]);
            return 1;
        }
    }
    return 0;
}

static int pick_same(const uint8_t *buf, void *ctx)
{
    return !memcmp(buf, ctx, REC_DATA);
}

/* verify: the game has loaded the save list; is slot s_slot's record there,
 * equal to a save file on disk, with both records signed? */
static int verify_check(void)
{
    static uint8_t buf[FILE_SIZE];
    uint8_t rec[REC_DATA];
    char name[260];
    uint32_t base = M32(0x00DE0BD8u + 0x10u);   /* channel 0's record buffers */
    uint32_t i;
    const char *root = save_root();
    if (!base) {
        snprintf(s_verify_why, sizeof s_verify_why, "the save list has no records");
        return 0;
    }
    for (i = 0; i < REC_DATA; i++)
        rec[i] = M8(base + (uint32_t)s_slot * REC_SIZE + i);
    if (!rec[0x2C]) {
        snprintf(s_verify_why, sizeof s_verify_why,
                 "the game did not load home save %d (no team name)", s_slot + 1);
        return 0;
    }
    if (!root) {
        snprintf(s_verify_why, sizeof s_verify_why,
                 "home save %d '%.32s' loads in the game (no RECOMP_SAVE_DIR: files "
                 "not checked)", s_slot + 1, (const char *)rec + 0x2C);
        return 1;
    }
    if (!each_save(pick_same, rec, buf, name, sizeof name)) {
        snprintf(s_verify_why, sizeof s_verify_why,
                 "home save %d '%.32s' in the game matches no save file", s_slot + 1,
                 (const char *)rec + 0x2C);
        return 0;
    }
    if (!record_signed(buf, 0)) {
        snprintf(s_verify_why, sizeof s_verify_why,
                 "home save %d (%s): the main record's signature is wrong", s_slot + 1, name);
        return 0;
    }
    /* The second record's signature is not always current (the human's
     * own slot 2 has a stale one and loads), so it is reported only. */
    snprintf(s_verify_why, sizeof s_verify_why,
             "home save %d '%.32s' (%s) loads in the game, main record signed%s",
             s_slot + 1, (const char *)rec + 0x2C, name,
             record_signed(buf + REC_SIZE, 1) ? "" : " (second record's signature stale)");
    return 1;
}

/* After a return: one line on what came back, from the newest home save. */
static void return_summary(void)
{
    static uint8_t buf[FILE_SIZE];
    static char names[MAX_SAVES][64];
    char path[700], best[64] = "";
    time_t newest = 0;
    FILE *f;
    int team, items[2] = { 0, 0 }, n, i;
    static const struct { uint16_t at, n; } cats[] = {
        { 0x74, 43 }, { 0xB4, 22 }, { 0xF4, 23 }, { 0x114, 38 } };
    n = save_folders(names, MAX_SAVES);
    for (i = 0; i < n; i++) {         /* by the file's time: a folder's does not move */
        struct stat st;
        data_path(path, sizeof path, names[i]);
        if (stat(path, &st) == 0 && st.st_mtime >= newest) {
            newest = st.st_mtime;
            snprintf(best, sizeof best, "%s", names[i]);
        }
    }
    if (!best[0])
        return;
    data_path(path, sizeof path, best);
    if (!(f = fopen(path, "rb")))
        return;
    if (fread(buf, 1, FILE_SIZE, f) == FILE_SIZE) {
        for (team = 0; team < 2; team++) {
            uint32_t blk = team ? 0x746Cu : 0x65C0u;
            unsigned c, k;
            for (c = 0; c < 4; c++)
                for (k = 0; k < cats[c].n; k++)
                    items[team] += buf[blk + cats[c].at + k] != 0;
        }
        fprintf(stderr, "[JBAG] summary: save %s '%.32s', gold Time Sweepers %d, "
                "Tom-Tom Gang %d, items %d / %d, main record signature %s\n", best,
                (const char *)buf + 0x2C, *(const int32_t *)(buf + 0x65D4),
                *(const int32_t *)(buf + 0x7480), items[0], items[1],
                record_signed(buf, 0) ? "good" : "WRONG");
    }
    fclose(f);
}

/* The next button for the state the menus are in, or K_NONE to wait. */
static int decide(void)
{
    uint32_t m = M32(G_MODE), mode = m & 0x7Fu;

    if (mode == 1 && (m & 0x80u)) {
        uint32_t ts = M32(TITLE), cur = M32(TITLE + 0x10);
        if (ts == 0) {
            /* With no usable home save, Start begins a new game (the
             * opening movie) and the menu never opens. */
            if (++s_pressed_start > 8 && !s_seen_menu) {
                finish(0, "the title menu never opened after 8 presses of Start "
                          "(no usable home save?)");
                return K_NONE;
            }
            return K_START;
        }
        s_seen_menu |= ts == 2;
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
        int want = s_job == JOB_RETURN ? 1 : 0;
        if (s_job == JOB_VERIFY) {
            /* Create's home-save list is the game reading every home save
             * (and checking its signature). Look, then back out to the
             * menu: nothing is written. */
            if (s_verify_result >= 0) {
                if (flow == 0) {
                    finish(s_verify_result, s_verify_why);
                    return K_NONE;
                }
                return K_B;
            }
            if (flow == 0)
                return cur != 0 ? K_DOWN : K_A;
            if (flow != 1)
                return K_B;
            if (scr != 1) {
                s_verify_settle = 0;
                return K_NONE;
            }
            if (++s_verify_settle < 4)  /* a few decisions on the list */
                return K_NONE;
            s_verify_result = verify_check();
            return K_B;
        }
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
                return_summary();
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
    /* Loading, or another mode. Seen for long after Start on the title,
     * the game went elsewhere: with no usable home save, Start begins a
     * new game (the opening movie) instead of opening the menu. */
    if (s_pressed_start && mode != 1 && mode != 0x1D && mode != 0) {
        if (++s_alien_polls > 600) {
            char why[120];
            snprintf(why, sizeof why, "the game went to mode 0x%X, not the title "
                     "menu (no usable home save?)", m);
            finish(0, why);
        }
    } else {
        s_alien_polls = 0;
    }
    return K_NONE;
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
        /* The same press on an unchanged menu, over and over: it does
         * nothing here. Seen with an unusable home save, where Prepare
         * for Battle cannot be selected on the title menu. */
        uint32_t now[5] = { M32(G_MODE), M32(TITLE + 0x10), M32(PFB_MENU), M32(PFB_SCREEN),
                            M32(TITLE) };
        if (s_key == s_last_key && !memcmp(now, s_last_state, sizeof now)) {
            if (++s_same_presses >= 10) {
                static const char *const names[] = { "", "up", "down", "Start", "A", "B" };
                char why[160];
                snprintf(why, sizeof why, "pressing %s changes nothing (mode 0x%X, cursor "
                         "%u, menu 0x%X, screen 0x%X; no usable home save?)",
                         names[s_key], now[0], now[1], now[2], now[3]);
                finish(0, why);
                return;
            }
        } else {
            s_same_presses = 0;
            s_last_key = s_key;
            memcpy(s_last_state, now, sizeof now);
        }
        s_hold = HOLD_POLLS - 1;
        press(st, s_key);
        s_wait_polls = 0;
    } else if (!s_finished && s_pressed_start) {
        /* Waiting with nothing to press while the menus do not move: the
         * game is somewhere this does not know -- for one, with no usable
         * home save Start begins a new game (the opening movie) inside
         * the title's own mode. Ten seconds of that is a failure. */
        uint32_t now[4] = { M32(G_MODE), M32(TITLE), M32(PFB_MENU), M32(PFB_SCREEN) };
        if (memcmp(now, s_wait_state, sizeof now)) {
            memcpy(s_wait_state, now, sizeof now);
            s_wait_polls = 0;
        } else if (++s_wait_polls > 600) {
            char why[160];
            snprintf(why, sizeof why, "stuck with nothing to press (mode 0x%X, title "
                     "state %u, menu 0x%X, screen 0x%X; no usable home save?)",
                     now[0], now[1], now[2], now[3]);
            finish(0, why);
        }
    }
}
