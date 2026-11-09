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
#include <math.h>
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
#include "netplay.h"                  /* xbox_NetplayActive/Player */
#include "usb_gamepad.h"              /* usb_gamepad_set_filter */
#include "xbox_memory_layout.h"       /* KeTickCount's guest address */

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
enum { JOB_NONE, JOB_CREATE, JOB_RETURN, JOB_VERIFY, JOB_VS };
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
    } else if (!strcmp(e, "vs")) {
        s_job = JOB_VS;
    } else if (!strncmp(e, "verify:", 7) && (e[7] == '1' || e[7] == '2') && !e[8]) {
        s_job = JOB_VERIFY;
        s_slot = e[7] - '1';
    } else {
        char why[200];
        snprintf(why, sizeof why, "RECOMP_JOURNEYBAG=%.60s (want create:1, create:2, "
                 "create:<slot>:overwrite, return, verify:<slot> or vs)", e);
        finish(0, why);               /* exits 5 with RECOMP_JOURNEYBAG_EXIT */
        return;
    }
    if (s_job == JOB_VS) {
        fprintf(stderr, "[JBAG] vs: player 1 saved game, player 2 Journey Bag on MU 2A, "
                "default rules, then out to the title\n");
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
/* The subfolder names of dir (up to max). */
static int list_dirs(const char *dir, char names[][64], int max)
{
    int n = 0;
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

/* The home save folders under the title's UDATA directory. */
static int save_folders(char names[][64], int max)
{
    char dir[600];
    const char *root = save_root();
    if (!root)
        return 0;
    snprintf(dir, sizeof dir, "%s" PATH_SEP TITLE_DIR, root);
    return list_dirs(dir, names, max);
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

/* ── vs: one 2-player Vs. match, player 1 from a saved game, player 2 from
 * the Journey Bag on MU 2A, default rules, played to its end, then out to
 * the title: for checking what a battle writes to a bag.
 *
 * Vs state (memory dumps of a walk, 2026-11-09):
 *   mode 0x1A (Vs menus); [0xEB9E58] screen: 0 join, 2 team selection,
 *     1 rules (cursor starts on Proceed to Match), 4 into the match
 *   player p's struct 0xEB9ECC + p*0xB8:
 *     +0x00 0 picking a team, 1 team screen (cursor starts on Complete
 *           Selection), 5 ready
 *     +0x08 while picking: 1 select guest team (+0x20 cursor: 0 a preset
 *           team, 1 load from saved game, 2 load from Journey Bag; starts
 *           on 0), 2 save list, 4 memory-unit list (+0x28 the card; A is
 *           accepted when the card's entry 0xDE0818 + card*0x78 has status
 *           3 = holds a bag and +0x68 = 0 = not mounted, sub_00201BB0
 *           0x20230B), 5 reading the bag, 6 the bag's team list
 *   These menus move with the left stick, not the d-pad.
 * In the match (any other mode) both press A now and then: NEXT ROUND on
 * the between-round menu and the result screens; in battle A is harmless.
 * Nobody scores, so rounds tie and end on time. */
#define VS_PLAYER(p)  (0x00EB9ECCu + (uint32_t)(p) * 0xB8u)
#define VS_SCREEN     0x00EB9E58u
#define VS_RULES_CURSOR 0x00EB9E88u   /* rules screen: 0 Proceed to Match .. 4 Number of Battles */
#define VS_SETS       0x00EB9E9Cu     /* Number of Battles: sets - 1 */
enum { V_NONE, V_A, V_B, V_START, V_LUP, V_LDOWN, V_UP, V_DOWN, V_LLEFT, V_LRIGHT };
/* The vs driver runs in the pad filter (usb_gamepad_set_filter): it sees
 * each pad's report at the USB poll rate, or once a frame for this machine's
 * own player in a network game. So its presses are timed by the guest's
 * KeTickCount (ms; under a schedule it moves by recorded or virtual time,
 * the same on every peer), and a press ends early once the player's menu
 * state changes -- a held stick would otherwise repeat. */
#define VS_TICK      (XBOX_KERNEL_DATA_BASE + KDATA_TICK_COUNT)
#define VS_HOLD_MS   100u             /* a press: held at most this long */
#define VS_GAP_MS    200u             /* then released this long */
static int s_vs_key[2];
static uint32_t s_vs_hold_until[2], s_vs_gap_until[2], s_vs_spam[2], s_vs_snap[2];
static uint32_t s_vs_ms, s_vs_t0;       /* ms since the job started */
static int s_vs_t0_set;
static int s_vs_matched, s_vs_back;

/* s: the 18 state bytes of a pad report (usb_gamepad.h). */
static void vs_press(uint8_t *s, int key)
{
    switch (key) {
    case V_A:     s[2] = 0xFF; break;
    case V_B:     s[3] = 0xFF; break;
    case V_START: s[0] |= 0x10; break;
    case V_UP:    s[0] |= 0x01; break;
    case V_DOWN:  s[0] |= 0x02; break;
    case V_LUP:   s[12] = 0xFF; s[13] = 0x7F; break;    /* left stick Y +32767 */
    case V_LDOWN: s[12] = 0x01; s[13] = 0x80; break;    /* -32767 */
    case V_LRIGHT: s[10] = 0xFF; s[11] = 0x7F; break;   /* left stick X */
    case V_LLEFT:  s[10] = 0x01; s[11] = 0x80; break;
    default: break;
    }
}

/* When was each save file last written, home saves and the bag on MU 2A. */
static void vs_file_report(const char *when)
{
    static char names[MAX_SAVES][64];
    char path[700];
    struct stat st;
    const char *root = save_root();
    int n, i, u;
    if (!root)
        return;
    n = save_folders(names, MAX_SAVES);
    for (i = 0; i < n; i++) {
        data_path(path, sizeof path, names[i]);
        if (stat(path, &st) == 0)
            fprintf(stderr, "[JBAG] vs %s: home save %s last written %ld\n", when,
                    names[i], (long)st.st_mtime);
    }
    for (u = 0; u < 8; u++) {         /* bags: <save>/MU/<n>/UDATA/<title>/<folder> */
        char dir[620];
        snprintf(dir, sizeof dir, "%s" PATH_SEP "MU" PATH_SEP "%d" PATH_SEP TITLE_DIR, root, u);
        n = list_dirs(dir, names, MAX_SAVES);
        for (i = 0; i < n; i++) {
            snprintf(path, sizeof path, "%s" PATH_SEP "%s" PATH_SEP "blinx2data.bin", dir,
                     names[i]);
            if (stat(path, &st) == 0)
                fprintf(stderr, "[JBAG] vs %s: MU %d save %s last written %ld\n", when, u,
                        names[i], (long)st.st_mtime);
        }
    }
}

/* The next press for player p (0/1), or V_NONE. */
static int vs_decide(int p)
{
    uint32_t m = M32(G_MODE), mode = m & 0x7Fu;
    if (mode == 1 && (m & 0x80u)) {
        uint32_t ts = M32(TITLE), cur = M32(TITLE + 0x10);
        if (s_vs_matched) {
            if (p == 0) {
                vs_file_report("back at the title");
                finish(1, "vs match played to its end; back at the title");
            }
            return V_NONE;
        }
        if (p != 0)
            return V_NONE;
        if (ts == 0)
            return V_START;
        if (ts != 2)
            return V_NONE;
        switch (cur) {
        case 0: return V_DOWN;        /* Story Mode -> Vs. Mode */
        case 1: return V_A;
        case 6: return V_A;           /* Vs. Play */
        case 7: return V_UP;
        default: return V_B;
        }
    }
    if (mode == 0x1D && (m & 0x80u))  /* Prepare for Battle by a slip: back out */
        return p == 0 ? V_B : V_NONE;
    if (mode == 0x1A && (m & 0x80u)) {
        uint32_t scr = M32(VS_SCREEN);
        uint32_t ph = M32(VS_PLAYER(p)), sub = M32(VS_PLAYER(p) + 8);
        if (s_vs_matched) {
            /* After the match the game is back on team selection. Backing
             * out from there warns "you and your friends' game will not be
             * saved", so the save comes from going on. To see which step
             * writes what: wait there RECOMP_JB_VS_WAIT ms (default 5000),
             * then player 1 completes the selection, then player 2;
             * the file times are reported after each step, and at the rules
             * screen the job ends. */
            static uint32_t wait = 0xFFFFFFFFu, t0;
            static int step;
            uint32_t ph0 = M32(VS_PLAYER(0));
            if (wait == 0xFFFFFFFFu) {
                const char *e = getenv("RECOMP_JB_VS_WAIT");
                wait = e ? (uint32_t)strtoul(e, NULL, 0) : 5000u;
            }
            if (!s_vs_back) {
                s_vs_back = 1;
                t0 = s_vs_ms;
                vs_file_report("match over, team selection");
            }
            if (scr == 1) {
                if (p == 0) {
                    vs_file_report("selection completed again (rules screen)");
                    finish(1, "vs match played to its end and the selection completed "
                              "again; see the file times");
                }
                return V_NONE;
            }
            if (scr != 2 || s_vs_ms - t0 < wait)
                return V_NONE;
            if (step == 0 && p == 0) {
                step = 1;
                vs_file_report("waited on team selection");
            }
            if (step == 1 && ph0 != 1) {
                step = 2;
                t0 = s_vs_ms;      /* player 1 done; wait again before player 2 */
                return V_NONE;
            }
            if (step == 2 && p == 0 && s_vs_ms - t0 >= wait) {
                step = 3;
                vs_file_report("player 1 completed the selection");
            }
            if (ph == 1 && ((p == 0 && step == 1) || (p == 1 && step == 3)))
                return V_A;           /* Complete Selection */
            return V_NONE;
        }
        if (scr == 0)                 /* both join, then player 1 starts */
            return p == 0 ? ((s_vs_ms / 700) & 1 ? V_START : V_A) : V_A;
        if (scr == 1) {               /* rules: Number of Battles, Proceed to Match */
            static int stop = -1;     /* RECOMP_JB_VS_STOP=rules: stay (exploring) */
            static uint32_t want;     /* RECOMP_JB_VS_SETS (1): [VS_SETS] = sets - 1 */
            uint32_t cur = M32(VS_RULES_CURSOR), val = M32(VS_SETS);
            if (stop < 0) {
                const char *e = getenv("RECOMP_JB_VS_SETS");
                int n = e ? atoi(e) : 1;
                stop = getenv("RECOMP_JB_VS_STOP") != NULL;
                want = n > 1 ? (uint32_t)(n - 1) : 0u;
            }
            if (p != 0 || stop)
                return V_NONE;
            if (val != want) {        /* cursor 4 = Number of Battles */
                if (cur != 4)
                    return cur < 4 ? V_LDOWN : V_LUP;
                return val > want ? V_LLEFT : V_LRIGHT;
            }
            if (cur != 0)             /* 0 = Proceed to Match */
                return V_LUP;
            return V_A;
        }
        if (scr != 2 || ph == 5)
            return V_NONE;
        if (ph == 1)
            return V_A;               /* Complete Selection */
        if (ph != 0)
            return V_NONE;
        switch (sub) {
        case 1: {                     /* select guest team: 0 a preset team, */
            /* 1 load from saved game (player 1), 2 load from Journey Bag
             * (player 2); the cursor starts on 0 */
            static int swap = -1;     /* RECOMP_JB_VS_SWAP=1: player 1 takes the bag */
            uint32_t want, cur = M32(VS_PLAYER(p) + 0x20);
            if (swap < 0)
                swap = getenv("RECOMP_JB_VS_SWAP") != NULL;
            want = (p != 0) != (swap != 0) ? 2u : 1u;
            if (cur != want)
                return cur < want ? V_LDOWN : V_LUP;
            return V_A;
        }
        case 2: case 3: case 4: case 6:   /* save list, card list (4), bag teams */
            return V_A;
        default:
            return V_NONE;
        }
    }
    if (mode == 0x0A && (m & 0x80u))
        s_vs_matched = 1;             /* the match itself (mode 10, as stages) */
    if (s_vs_matched && s_vs_ms - s_vs_spam[p] >= 2000u) {   /* the match and its results */
        s_vs_spam[p] = s_vs_ms;                             /* screens: A now and then */
        return V_A;
    }
    return V_NONE;
}

/* What a press can change for player p: a hash of the menu state. */
static uint32_t vs_snap(int p)
{
    uint32_t h = M32(G_MODE), i;
    static const uint32_t at[] = { TITLE, TITLE + 0x10, VS_SCREEN, VS_RULES_CURSOR, VS_SETS };
    for (i = 0; i < 5; i++)
        h = h * 31u + M32(at[i]);
    static const uint32_t pl[] = { 0x00, 0x08, 0x20, 0x28 };  /* phase, sub, cursor, card */
    for (i = 0; i < 4; i++)
        h = h * 31u + M32(VS_PLAYER(p) + pl[i]);
    return h;
}

/* RECOMP_JB_VS_FIGHT=<buttons> (e.g. "x", "b+rt"): during the match player 2
 * (or player 1 with RECOMP_JB_VS_SWAP) walks to the nearest member of the
 * other team that still has HP and holds those buttons 400 ms of every
 * 1000 ms; the other player stands. All from guest state: actors at
 * 0xCD2440 + i*0x30C (player p's team is actors 4p..4p+3; +0x6C word HP,
 * +0xEC/+0xF4 float x/z), player p's camera [0x4B2718 + p*4] (+0x10 yaw;
 * the stick's up is (-sin yaw, cos yaw) in x/z, its right (cos yaw, sin yaw)). */
#define VS_ACTOR(i)   (0x00CD2440u + (uint32_t)(i) * 0x30Cu)
#define VS_CAMERAS    0x004B2718u
static float vs_f32(uint32_t va)
{
    uint32_t v = M32(va);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

#define VS_PICKUPS    0x00B22E68u     /* 256 objects of 0x6C: +2 type, +0x14 position */

/* The stick toward world direction (dx, dz) for player p's camera. */
static void vs_stick_to(int p, float dx, float dz, uint8_t *s18)
{
    uint32_t cam = M32(VS_CAMERAS + (uint32_t)p * 4u);
    float yaw, len = sqrtf(dx * dx + dz * dz), up, right;
    int16_t sx, sy;
    if (!cam || len < 1e-3f)
        return;
    yaw = vs_f32(cam + 0x10);
    dx /= len;
    dz /= len;
    up = -sinf(yaw) * dx + cosf(yaw) * dz;
    right = cosf(yaw) * dx + sinf(yaw) * dz;
    sx = (int16_t)(right * 32767.0f);
    sy = (int16_t)(up * 32767.0f);
    s18[10] = (uint8_t)sx; s18[11] = (uint8_t)((uint16_t)sx >> 8);
    s18[12] = (uint8_t)sy; s18[13] = (uint8_t)((uint16_t)sy >> 8);
}

static void vs_fight(int p, uint8_t *s18)
{
    static const char *const names[8] = { "a", "b", "x", "y", "black", "white", "lt", "rt" };
    static int fighter = -2, probe, detour_sign = 1;
    static uint8_t buttons;           /* bit n = analog byte s18[2 + n] */
    static uint32_t last_log, last_check, detour_until;
    static float last_x, last_z;
    static uint8_t skip[256];         /* junk that would not come */
    static int near_junk = -1, last_target = -1, detours;
    static uint32_t near_since, target_since;
    uint32_t m = M32(G_MODE), tgt_va = 0;
    float mx, my, mz, best = 1e30f, tx = 0, tz = 0, dx, dz, dist;
    int i, me = 4 * p, other = 4 * (1 - p), target = -1, held = -1;
    uint32_t ammo;
    if (fighter == -2) {
        const char *e = getenv("RECOMP_JB_VS_FIGHT");
        fighter = -1;
        if (e) {
            fighter = getenv("RECOMP_JB_VS_SWAP") ? 0 : 1;
            probe = !strcmp(e, "probe");
            for (i = 0; i < 8 && !probe; i++) {
                const char *f = strstr(e, names[i]);
                size_t n = strlen(names[i]);
                while (f && ((f != e && f[-1] != '+') || (f[n] && f[n] != '+')))
                    f = strstr(f + 1, names[i]);
                if (f)
                    buttons |= (uint8_t)(1u << i);
            }
        }
    }
    if (fighter < 0 || !s_vs_matched || (m & 0x7Fu) != 0x0A || !(m & 0x80u))
        return;
    if (p != fighter) {               /* the other player walks to the fighter */
        float ox = vs_f32(VS_ACTOR(4 * fighter) + 0xEC) - vs_f32(VS_ACTOR(me) + 0xEC);
        float oz = vs_f32(VS_ACTOR(4 * fighter) + 0xF4) - vs_f32(VS_ACTOR(me) + 0xF4);
        if (ox * ox + oz * oz > 40.0f * 40.0f)
            vs_stick_to(p, ox, oz, s18);
        return;
    }
    my = vs_f32(VS_ACTOR(me) + 0xF0);
    mx = vs_f32(VS_ACTOR(me) + 0xEC);
    mz = vs_f32(VS_ACTOR(me) + 0xF4);
    ammo = (M32(VS_ACTOR(me) + 0x44) >> 16) & 0xFF;     /* +0x46: junk loaded */
    if (probe && !ammo) {             /* the nearest junk (pickup types 1-0x23) */
        for (i = 0; i < 256; i++) {
            uint32_t o = VS_PICKUPS + (uint32_t)i * 0x6Cu;
            uint32_t t = (M32(o) >> 16) & 0xFF;
            float x, z, d;
            if (t < 1 || t > 0x23 || skip[i] || (M32(o + 4) & 0xFF) != 0xFF ||
                    (M32(o + 0xC) >> 24) != 1)
                continue;                 /* +4 0xFF and +0xF 1: lying there */
            x = vs_f32(o + 0x14);
            z = vs_f32(o + 0x1C);
            d = vs_f32(o + 0x18) - my;    /* height counts 4x: ledges */
            d = (x - mx) * (x - mx) + (z - mz) * (z - mz) + 16.0f * d * d;
            if (d < best) {
                best = d;
                tx = x;
                tz = z;
                target = i;
                tgt_va = o;
            }
        }
    } else {
        for (i = other; i < other + 4; i++) {
            float x = vs_f32(VS_ACTOR(i) + 0xEC), z = vs_f32(VS_ACTOR(i) + 0xF4);
            float d = (x - mx) * (x - mx) + (z - mz) * (z - mz);
            if ((int16_t)(M32(VS_ACTOR(i) + 0x6C) & 0xFFFF) > 0 && d < best) {
                best = d;
                tx = x;
                tz = z;
                target = i;
            }
        }
    }
    if (target < 0)
        return;
    if (target != last_target) {
        last_target = target;
        target_since = s_vs_ms;
        detours = 0;
    } else if (tgt_va && (detours > 4 || s_vs_ms - target_since > 15000u)) {
        skip[target] = 1;             /* junk out of reach: the next one */
        last_target = -1;
    }
    dist = sqrtf((tx - mx) * (tx - mx) + (tz - mz) * (tz - mz));
    dx = tx - mx;
    dz = tz - mz;
    /* Stuck (under 8 units in 700 ms while far from the target): go sideways
     * for 1.2 s, alternating sides, with a jump. */
    if (s_vs_ms - last_check >= 700u) {
        float moved = sqrtf((mx - last_x) * (mx - last_x) + (mz - last_z) * (mz - last_z));
        if (moved < 8.0f && dist > 30.0f && s_vs_ms >= detour_until) {
            detour_until = s_vs_ms + 1200u;
            detour_sign = -detour_sign;
            detours++;
        }
        last_check = s_vs_ms;
        last_x = mx;
        last_z = mz;
    }
    if (s_vs_ms < detour_until) {
        float rx = -dz * (float)detour_sign, rz = dx * (float)detour_sign;
        dx = rx;
        dz = rz;
        if (detour_until - s_vs_ms > 1000u)
            s18[2] = 0xFF;            /* A: jump */
    }
    vs_stick_to(p, dx, dz, s18);
    if (probe && !ammo && dist < 15.0f) {   /* at the junk: face it, barely moving */
        int16_t sx = (int16_t)(s18[10] | (s18[11] << 8)), sy = (int16_t)(s18[12] | (s18[13] << 8));
        sx /= 4;
        sy /= 4;
        s18[10] = (uint8_t)sx; s18[11] = (uint8_t)((uint16_t)sx >> 8);
        s18[12] = (uint8_t)sy; s18[13] = (uint8_t)((uint16_t)sy >> 8);
    }
    if (probe && !ammo) {
        if (dist < 40.0f) {
            s18[2 + 7] = 0xFF;        /* RT: vacuum */
            if (near_junk != target) {
                near_junk = target;
                near_since = s_vs_ms;
            } else if (s_vs_ms - near_since > 6000u) {
                skip[target] = 1;     /* 6 s and still there: another one */
                near_junk = -1;
            }
        }
    } else if (probe) {               /* loaded: tap each button in turn */
        held = 1 + (int)((s_vs_ms / 2000u) % 7u);       /* b x y black white lt rt */
        if (s_vs_ms % 2000u < 200u)
            s18[2 + held] = 0xFF;
        else
            held = -1;
    } else if (s_vs_ms % 1000u < 400u) {
        for (i = 0; i < 8; i++)
            if (buttons & (1u << i))
                s18[2 + i] = 0xFF;
    }
    if (s_vs_ms - last_log >= (probe ? 250u : 1000u)) {
        int hp_other = 0, hp_me = 0;
        char ab[3 * 16 + 1];
        last_log = s_vs_ms;
        for (i = 0; i < 4; i++) {
            hp_other += (int16_t)(M32(VS_ACTOR(other + i) + 0x6C) & 0xFFFF);
            hp_me += (int16_t)(M32(VS_ACTOR(me + i) + 0x6C) & 0xFFFF);
        }
        for (i = 0; i < 16; i++)
            snprintf(ab + 3 * i, 4, "%02X ", (M32(VS_ACTOR(me) + 0x40 + (uint32_t)(i & ~3)) >>
                                               (8 * (i & 3))) & 0xFF);
        fprintf(stderr, "[JBAG] vs fight ms %u: player %d at %.0f,%.0f -> %s %d at %.0f,%.0f "
                "(%.0f away)%s; held %s; team HP %d vs %d; gold %u; +0x40: %s; target +0x5C %08X\n",
                s_vs_ms, p + 1, mx, mz, probe ? "junk" : "actor", target, tx, tz, dist,
                s_vs_ms < detour_until ? " detour" : "", held >= 0 ? names[held] : "-", hp_me, hp_other, M32(VS_ACTOR(me) + 0xDC) & 0xFFFF, ab,
                tgt_va ? M32(tgt_va + 0x5C) : 0u);
    }
}

/* The pad filter: pad's 18 report bytes, before the schedule, the input
 * recording and a network game see them. Decides only from guest state. */
static void vs_filter(int pad, uint8_t *s18)
{
    uint32_t c;
    int p = xbox_NetplayActive() ? xbox_NetplayPlayer() : pad;
    if (s_finished || input_rec_mode() == 2)
        return;
    c = M32(VS_TICK);                 /* the guest clock */
    if (!s_vs_t0_set) {
        s_vs_t0_set = 1;
        s_vs_t0 = c;
    }
    s_vs_ms = c - s_vs_t0;
    if (p < 0 || p > 1)
        return;
    if (s_vs_ms > 40u * 60u * 1000u) {
        char why[120];
        snprintf(why, sizeof why, "vs: no end after 40 minutes (mode 0x%X, screen %u)",
                 M32(G_MODE), M32(VS_SCREEN));
        finish(0, why);
        return;
    }
    vs_fight(p, s18);                 /* RECOMP_JB_VS_FIGHT: steering and fire */
    if (s_vs_hold_until[p]) {
        if (s_vs_ms < s_vs_hold_until[p] && vs_snap(p) == s_vs_snap[p]) {
            vs_press(s18, s_vs_key[p]);
            return;
        }
        s_vs_hold_until[p] = 0;
        s_vs_gap_until[p] = s_vs_ms + VS_GAP_MS;
    }
    if (s_vs_ms < s_vs_gap_until[p])
        return;
    s_vs_key[p] = vs_decide(p);
    if (s_vs_key[p] != V_NONE && !s_finished) {
        static int trace = -1;        /* RECOMP_JB_VS_TRACE=1: every press */
        if (trace < 0)
            trace = getenv("RECOMP_JB_VS_TRACE") != NULL;
        if (trace)
            fprintf(stderr, "[JBAG] vs press p%d key %d ms %u mode 0x%X title %u/%u "
                    "screen %u phase %u sub %u cursor %u\n", p, s_vs_key[p], s_vs_ms,
                    M32(G_MODE), M32(TITLE), M32(TITLE + 0x10), M32(VS_SCREEN),
                    M32(VS_PLAYER(p)), M32(VS_PLAYER(p) + 8), M32(VS_PLAYER(p) + 0x20));
        s_vs_hold_until[p] = s_vs_ms + VS_HOLD_MS;
        s_vs_snap[p] = vs_snap(p);
        vs_press(s18, s_vs_key[p]);
    }
}

void journeybag_init(void)
{
    if (s_job < 0)
        read_job();
    if (s_job == JOB_VS)
        usb_gamepad_set_filter(vs_filter);
}

void journeybag_pre_poll(uint32_t handle, uint8_t *st)
{
    if (s_job < 0)
        read_job();
    if (!s_job || s_finished || !st || input_rec_mode() == 2)
        return;
    if (s_job == JOB_VS)
        return;                       /* the pad filter drives it (vs_filter) */
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
