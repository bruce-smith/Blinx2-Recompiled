/**
 * Blinx 2 cheat engine (see cheats.h for the API and notes-cheats.md for the
 * reverse-engineered game state it uses).
 *
 * Every cheat request from the UI is queued; the game thread applies it at
 * its next pad poll (cheats_on_poll, called from the XInputGetState override
 * in recomp_manual.c). That is a point inside the game's own frame where it
 * calls out to XAPI, so writing game state there, or calling one of the
 * game's own routines, cannot race the game's threads.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_MSC_VER)
#include <intrin.h>
#define CHEATS_TLS __declspec(thread)
#else
#define CHEATS_TLS __thread
#define _InterlockedExchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#endif

#include "cheats.h"
#include "xbox_ui.h"        /* the in-game menu's panel */

extern ptrdiff_t g_xbox_mem_offset;
extern CHEATS_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern CHEATS_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern CHEATS_TLS double g_fp_stack[8];
extern CHEATS_TLS int g_fp_top;

/* recomp_manual.c: the input recording */
int  input_rec_mode(void);          /* 0 off, 1 recording, 2 replaying */
void input_rec_cheat_event(const uint8_t *data);

#define M8(va)   (*(volatile uint8_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M16(va)  (*(volatile int16_t  *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define M32(va)  (*(volatile uint32_t *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))
#define MF(va)   (*(volatile float    *)((uintptr_t)(uint32_t)(va) + g_xbox_mem_offset))

/* ── Game state (notes-cheats.md) ───────────────────────────────────── */

#define G_MODE          0x00AE73F0u   /* main loop mode; bit 0x80 = initialised */
#define G_STAGE_CHANGE  0x00AE46ECu   /* 2 = a stage change is pending */
#define G_STAGE         0x00B871ACu   /* stage index being played */
#define G_PLAY_TYPE     0x00B21CB4u   /* 0 Sweeper, 1 Tom-Tom, 2/3 VS */
#define G_RECORD        0x005F4718u   /* -> live progress record (0x84C8 bytes) */
#define REC_MONEY_SWE   0x65D4u       /* team block 0x65C0 + 0x14 */
#define REC_MONEY_TOM   0x7480u       /* team block 0x746C + 0x14 */
#define MONEY_MAX       99999
#define SHOP_MONEY_PTR  0x00DE0450u   /* shop: -> the record money it edits */
#define SHOP_MONEY      0x00DE031Cu   /* shop: its working copy */
#define ACTORS          0x00CD2440u   /* characters, 0x30C each */
#define ACTOR_SIZE      0x30Cu
#define ACTOR_COUNT     8u            /* 2 teams of 4 slots */
#define A_CPU           0x07u         /* byte: 0 human, 1 CPU */
#define A_SIDE          0x2Fu         /* byte: P1's is the team played */
#define A_HP            0x6Cu         /* int16 */
#define A_HP_MAX        0x6Eu         /* int16 */
#define MODE_PLAY       10u           /* hub and stages */

/* The game's "go to stage": stage in edi, cdecl (int via_mode9). */
extern void sub_00062AE0(void);

int cheats_in_game(void)
{
    uint32_t m = M32(G_MODE);
    return (m & 0x7Fu) == MODE_PLAY && (m & 0x80u) && M32(G_STAGE_CHANGE) != 2;
}

static int team_now(void)
{
    uint8_t s = M8(ACTORS + A_SIDE);
    return s == 1 ? 1 : 0;
}

/* ── Requests (any thread) and their recorded form ─────────────────────
 *
 * A request is 22 bytes, the payload of an input-recording event: op, then
 * little-endian arguments. The UI fills the queue; the game thread drains
 * it at the poll, applies each and records it. */

enum { OP_NONE, OP_MONEY, OP_INVINCIBLE, OP_WARP, OP_FREECAM, OP_CAMPOSE, OP_ITEM, OP_AMMO };

typedef struct { uint8_t b[22]; } Req;
#define QLEN 32
static Req s_q[QLEN];
static volatile long s_q_head, s_q_tail;     /* single consumer (game thread) */
static volatile long s_q_lock;

static void q_push(const Req *r)
{
    while (_InterlockedExchange(&s_q_lock, 1))
        ;
    if (s_q_head - s_q_tail < QLEN) {
        s_q[s_q_head % QLEN] = *r;
        s_q_head++;
    }
    _InterlockedExchange(&s_q_lock, 0);
}

static int q_pop(Req *r)
{
    int got = 0;
    while (_InterlockedExchange(&s_q_lock, 1))
        ;
    if (s_q_tail < s_q_head) {
        *r = s_q[s_q_tail % QLEN];
        s_q_tail++;
        got = 1;
    }
    _InterlockedExchange(&s_q_lock, 0);
    return got;
}

static int ui_allowed(void)
{
    /* a replay decides what happens; the UI's requests would desync it */
    return input_rec_mode() != 2;
}

static void req(uint8_t op, int32_t a, int32_t b)
{
    Req r;
    if (!ui_allowed())
        return;
    memset(&r, 0, sizeof r);
    r.b[0] = op;
    memcpy(r.b + 1, &a, 4);
    memcpy(r.b + 5, &b, 4);
    q_push(&r);
}

/* ── State the game thread owns ──────────────────────────────────────── */

static volatile int s_invincible, s_ammo;
static volatile int s_ui_ammo;
static volatile int s_freecam, s_freecam_freeze;
static volatile int s_ui_invincible, s_ui_freecam;   /* what the UI asked for */

/* free camera pose: eye, pitch, yaw (the game's camera struct's angles) */
static float s_cam[5];
static int   s_cam_valid;
static int16_t s_stick[4];
static uint8_t s_trig[2], s_btn_a;

/* ── Money ── */

int cheats_get_money(int team)
{
    uint32_t rec = M32(G_RECORD);
    if (!rec)
        return -1;
    if (team < 0)
        team = team_now();
    return (int)M32(rec + (team ? REC_MONEY_TOM : REC_MONEY_SWE));
}

void cheats_give_money(int team, int amount)
{
    req(OP_MONEY, team, amount);
}

static int apply_money(int team, int amount)
{
    uint32_t rec = M32(G_RECORD), at;
    int64_t v;
    if (!rec)
        return 0;
    if (team < 0)
        team = team_now();
    at = rec + (team ? REC_MONEY_TOM : REC_MONEY_SWE);
    v = (int64_t)(int32_t)M32(at) + amount;
    v = v < 0 ? 0 : v > MONEY_MAX ? MONEY_MAX : v;
    M32(at) = (uint32_t)v;
    /* An open shop edits a copy and writes it back on exit; give it too. */
    if (M32(SHOP_MONEY_PTR) == at) {
        int64_t s = (int64_t)(int32_t)M32(SHOP_MONEY) + amount;
        M32(SHOP_MONEY) = (uint32_t)(s < 0 ? 0 : s > MONEY_MAX ? MONEY_MAX : s);
    }
    fprintf(stderr, "  [CHEAT] money: %s %+d -> %d\n", team ? "Tom-Toms" : "Sweepers",
            amount, (int)v);
    return 1;
}

/* ── Invincibility ── */

void cheats_set_invincible(int on)
{
    s_ui_invincible = on != 0;
    req(OP_INVINCIBLE, on != 0, 0);
}

int cheats_invincible(void)
{
    return input_rec_mode() == 2 ? s_invincible : s_ui_invincible;
}

static int is_human_actor(uint32_t a)
{
    uint32_t off = a - ACTORS;
    return off < ACTOR_SIZE * ACTOR_COUNT && off % ACTOR_SIZE == 0
        && M8(a + A_CPU) == 0;
}

/* The game's hit routine (sub_0019B790) asks before it hurts anyone. */
int cheats_block_damage(uint32_t victim)
{
    return s_invincible && is_human_actor(victim);
}

static void invincible_tick(void)
{
    uint32_t i;
    if (!s_invincible || !cheats_in_game())
        return;
    for (i = 0; i < ACTOR_COUNT; i++) {
        uint32_t a = ACTORS + i * ACTOR_SIZE;
        int16_t hp = M16(a + A_HP), mx = M16(a + A_HP_MAX);
        if (M8(a + A_CPU) == 0 && mx > 0 && hp > 0 && hp < mx)
            M16(a + A_HP) = mx;
    }
}

/* ── Infinite ammo ──
 *
 * A human actor's +0x46 byte is the ammo of the weapon it holds (+0x43,
 * the weapon's item index: 19 = slingshot, "20/20" on the HUD). While on,
 * each human actor's ammo is held at the highest value seen for the weapon
 * it holds, so it never goes down; a weapon change starts over. */
#define A_WEAPON 0x43u
#define A_AMMO   0x46u

void cheats_set_infinite_ammo(int on)
{
    s_ui_ammo = on != 0;
    req(OP_AMMO, on != 0, 0);
}

int cheats_infinite_ammo(void)
{
    return input_rec_mode() == 2 ? s_ammo : s_ui_ammo;
}

static void ammo_tick(void)
{
    static uint8_t weapon[ACTOR_COUNT], top[ACTOR_COUNT];
    uint32_t i;
    if (!s_ammo || !cheats_in_game()) {
        memset(top, 0, sizeof top);
        return;
    }
    for (i = 0; i < ACTOR_COUNT; i++) {
        uint32_t a = ACTORS + i * ACTOR_SIZE;
        uint8_t w = M8(a + A_WEAPON), n = M8(a + A_AMMO);
        if (M8(a + A_CPU) != 0)
            continue;
        if (w != weapon[i] || n > top[i]) {
            weapon[i] = w;
            top[i] = n;
        } else if (n < top[i])
            M8(a + A_AMMO) = top[i];
    }
}

/* ── Stage warp ──
 *
 * The game's stage table (debug names at 0x3F15C0) with the play type its
 * stage-select sets for each (sub_000133E0 at 0x156BF). */
typedef struct { const char *name; uint8_t idx, type; } Stage;
static const Stage s_stages[] = {
    { "1-1/1-2 Time Sweeper",      0, 0 }, { "1-3 Tom-Tom Gang",  2, 1 },
    { "1-4 Boss 1",                3, 0 }, { "1-4 VS",            3, 1 },
    { "2-1/2-2 Time Sweeper",      5, 0 }, { "2-3 Tom-Tom Gang",  7, 1 },
    { "2-4 Boss 2",                8, 0 }, { "2-4 VS",            8, 1 },
    { "3-1/3-2 Time Sweeper",     10, 0 }, { "3-3 Tom-Tom Gang", 12, 1 },
    { "3-4 Boss 3",               13, 0 }, { "3-4 VS",           13, 1 },
    { "4-1/4-2 Time Sweeper",     15, 0 }, { "4-3 Tom-Tom Gang", 17, 1 },
    { "4-4 Boss 4",               18, 0 }, { "4-4 VS",           18, 1 },
    { "5-1/5-2 Time Sweeper",     20, 0 }, { "5-3 Tom-Tom Gang", 22, 1 },
    { "5-4 Boss 5",               23, 0 }, { "5-4 VS",           23, 1 },
    { "6-1 Inside Boss 5",        25, 1 }, { "6-4 Last Boss",    28, 0 },
    { "Bonus round (Sweeper)",    40, 0 }, { "Bonus round (Tom-Tom)", 42, 1 },
    { "Last round boss",          43, 0 }, { "R09S02",           45, 0 },
    { "Time Factory (Sweeper hub)", 50, 0 }, { "Tom-Tom Base (Tom-Tom hub)", 55, 1 },
};
#define N_STAGES ((int)(sizeof s_stages / sizeof s_stages[0]))

int cheats_stage_count(void) { return N_STAGES; }

const char *cheats_stage_name(int i)
{
    return i >= 0 && i < N_STAGES ? s_stages[i].name : NULL;
}

int cheats_warp_stage(int i)
{
    if (i < 0 || i >= N_STAGES)
        return -1;
    req(OP_WARP, i, 0);
    return 0;
}

/* Call a lifted function from here, on the game thread, as if the game had:
 * registers and the x87 stack come back as they were. */
static void guest_call(void (*fn)(void), uint32_t edi, uint32_t arg)
{
    uint32_t ra = g_eax, rc = g_ecx, rd = g_edx, rb = g_ebx, rs = g_esi,
             rdi = g_edi, rbp = g_ebp, sp = g_esp;
    int top = g_fp_top;
    double fp[8];
    memcpy(fp, g_fp_stack, sizeof fp);
    g_edi = edi;
    g_esp -= 4;
    M32(g_esp) = arg;
    g_esp -= 4;
    M32(g_esp) = 0;                 /* return address: nobody looks */
    fn();                           /* pops its return address */
    g_eax = ra; g_ecx = rc; g_edx = rd; g_ebx = rb; g_esi = rs;
    g_edi = rdi; g_ebp = rbp; g_esp = sp;
    g_fp_top = top;
    memcpy(g_fp_stack, fp, sizeof fp);
}

static int apply_warp(int i)
{
    const Stage *s;
    if (i < 0 || i >= N_STAGES || !cheats_in_game())
        return 0;
    s = &s_stages[i];
    M32(G_PLAY_TYPE) = s->type;
    guest_call(sub_00062AE0, s->idx, 0);
    fprintf(stderr, "  [CHEAT] warp to %s (stage %u, type %u): %s\n", s->name,
            s->idx, s->type, M32(G_STAGE_CHANGE) == 2 ? "on its way" : "refused");
    return 1;
}

/* ── Items ──
 *
 * Shop/locker items are owned per team: each team block (record +0x65C0
 * Sweepers, +0x746C Tom-Toms) holds one state byte per item in each
 * category -- 0 not owned, 0x10 owned (what the new-game kit sets,
 * sub_0001E960; 2/4 are "just bought" marks the locker turns into 0x10,
 * sub_001C2310 / sub_0001FFD0). Definitions (0x4C bytes: price +0x30,
 * index +0x34, category +0x38) are in the tables below; names come from the
 * shop's icon names (e.g. shp_i_cleaner_fire) and models. */
typedef struct { uint8_t cat, idx; const char *name; } Item;
static const struct { uint16_t state; uint32_t defs; uint8_t n; } s_cats[] = {
    { 0x74,  0x4938D0u, 43 },   /* cleaners (Sweepers), weapons (Tom-Toms) */
    { 0xB4,  0x494598u, 22 },   /* gadgets and bombs */
    { 0xF4,  0x494C20u, 23 },   /* packs, attachments, food and drink */
    { 0x114, 0x4952F8u, 38 },   /* clothing and accessories */
};
static const Item s_items[] = {
    { 0, 1, "Cleaner Lv1" }, { 0, 2, "Cleaner Lv1 (black)" },
    { 0, 3, "Classic cleaner" }, { 0, 4, "Attack cleaner" },
    { 0, 5, "Time cleaner" }, { 0, 6, "Cleaner 2nd" }, { 0, 7, "V cleaner" },
    { 0, 8, "Fire cleaner" }, { 0, 9, "Freeze cleaner" },
    { 0, 10, "X-Blinx cleaner" }, { 0, 11, "W cleaner" }, { 0, 12, "Fish cleaner" },
    { 0, 13, "Cleaner 3rd" }, { 0, 14, "G cleaner" }, { 0, 15, "Legend cleaner" },
    { 0, 16, "Bazooka" }, { 0, 17, "Bat" }, { 0, 18, "Hammer" },
    { 0, 19, "Slingshot" }, { 0, 20, "Handgun" }, { 0, 21, "Machine gun" },
    { 0, 22, "Missile launcher S" }, { 0, 23, "Missile launcher M" },
    { 0, 24, "Ray gun" }, { 0, 25, "Flame gun" }, { 0, 26, "Rifle" },
    { 0, 27, "Shotgun" }, { 0, 28, "Sniper rifle" }, { 0, 29, "Water gun" },
    { 0, 30, "Landmine" }, { 0, 31, "Bomb switch" }, { 0, 32, "Grenade launcher" },
    { 0, 36, "Crossbow" }, { 0, 37, "Sol gun" }, { 0, 38, "Washtub" },
    { 0, 39, "Slipper" }, { 0, 41, "Rocket fist" }, { 0, 42, "Cat jar" },
    { 1, 1, "Tom-Tom decoy" }, { 1, 2, "Banana" }, { 1, 3, "Mochi" },
    { 1, 4, "Barrel" }, { 1, 5, "Time grenade" }, { 1, 6, "Smoke grenade" },
    { 1, 7, "Grenade" }, { 1, 8, "Bucket" }, { 1, 9, "Pit hole" },
    { 1, 10, "Dive hole" }, { 1, 12, "Bomber" }, { 1, 13, "Kutuki bomb" },
    { 1, 14, "Missile" }, { 1, 15, "Spike ball" }, { 1, 16, "Fire bomb" },
    { 1, 17, "Bomber x3" }, { 1, 18, "Kutuki bomb x3" }, { 1, 19, "Missile x3" },
    { 1, 20, "Spike ball x3" }, { 1, 21, "Fire bomb x3" },
    { 2, 3, "Sweeper pack S" }, { 2, 4, "Sweeper pack M" }, { 2, 5, "Sweeper pack L" },
    { 2, 6, "Combo cartridge" }, { 2, 7, "Silencer" }, { 2, 8, "Laser sight" },
    { 2, 10, "Smoothie" }, { 2, 12, "Sports drink" }, { 2, 13, "Cookies" },
    { 2, 14, "Sports drink (Tom-Tom)" }, { 2, 15, "Cookies (Tom-Tom)" },
    { 2, 16, "Meat S" }, { 2, 17, "Meat M" }, { 2, 18, "Meat L" },
    { 2, 19, "Milk S" }, { 2, 20, "Milk M" }, { 2, 21, "Milk L" },
    { 3, 1, "Goggles (red)" }, { 3, 2, "Goggles (blue)" }, { 3, 3, "Goggles (yellow)" },
    { 3, 4, "Goggles 2 (red)" }, { 3, 5, "Goggles 2 (blue)" },
    { 3, 6, "Goggles 2 (yellow)" }, { 3, 7, "Metal piercing" },
    { 3, 8, "Square piercing 1" }, { 3, 9, "Square piercing 2" },
    { 3, 10, "Square piercing 3" }, { 3, 11, "Square piercing 4" },
    { 3, 12, "Square piercing 5" }, { 3, 13, "Horn piercing 1" },
    { 3, 14, "Horn piercing 2" }, { 3, 15, "Horn piercing 3" },
    { 3, 16, "Horn piercing 4" }, { 3, 17, "Horn piercing 5" },
    { 3, 18, "Santa outfit" }, { 3, 19, "Tiger set (yellow)" },
    { 3, 20, "Tiger set (white)" }, { 3, 21, "Leopard set (brown)" },
    { 3, 22, "Leopard set (black)" }, { 3, 23, "Mask" },
    { 3, 24, "Spiral glasses" }, { 3, 25, "Surprise glasses" },
    { 3, 26, "Eye glasses" }, { 3, 27, "Heart glasses" }, { 3, 28, "Viking helmet" },
    { 3, 29, "Net helmet" }, { 3, 30, "Pirate hat" }, { 3, 31, "Sailor hat" },
    { 3, 33, "Raccoon cap" }, { 3, 34, "GS set" }, { 3, 35, "Leather set" },
    { 3, 36, "Baseball cap" }, { 3, 37, "Cleaning set" },
};
#define N_ITEMS ((int)(sizeof s_items / sizeof s_items[0]))
#define ITEM_OWNED 0x10u

int cheats_item_count(void) { return N_ITEMS; }

const char *cheats_item_name(int i)
{
    return i >= 0 && i < N_ITEMS ? s_items[i].name : NULL;
}

static uint32_t item_state(int team, int i)
{
    uint32_t rec = M32(G_RECORD);
    if (!rec || i < 0 || i >= N_ITEMS)
        return 0;
    return rec + (team ? 0x746Cu : 0x65C0u) + s_cats[s_items[i].cat].state + s_items[i].idx;
}

int cheats_item_owned(int team, int i)
{
    uint32_t at;
    if (team < 0)
        team = team_now();
    at = item_state(team, i);
    return at ? M8(at) != 0 : -1;
}

void cheats_give_item(int team, int i)
{
    if (i >= 0 && i < N_ITEMS)
        req(OP_ITEM, team, i);
}

void cheats_give_all_items(int team)
{
    req(OP_ITEM, team, -1);
}

static int apply_item(int team, int i)
{
    int k, n = 0;
    if (!M32(G_RECORD))
        return 0;
    if (team < 0)
        team = team_now();
    for (k = i < 0 ? 0 : i; k < N_ITEMS && (i < 0 || k == i); k++) {
        uint32_t at = item_state(team, k);
        if (at && M8(at) == 0) {
            M8(at) = ITEM_OWNED;
            n++;
        }
    }
    fprintf(stderr, "  [CHEAT] items: %s %s (%d new)\n", team ? "Tom-Toms" : "Sweepers",
            i < 0 ? "everything" : s_items[i].name, n);
    return 1;
}

/* ── Free camera ── */

void cheats_freecam_enable(int on, int freeze_player)
{
    s_ui_freecam = on != 0;
    req(OP_FREECAM, on != 0, freeze_player != 0);
}

int cheats_freecam_enabled(void)
{
    return input_rec_mode() == 2 ? s_freecam : s_ui_freecam;
}

void cheats_pre_poll(uint8_t *st)
{
    /* XINPUT_STATE: +4 buttons, +6 analog A, +12/+13 LT/RT, +14 sticks */
    if (!s_freecam || input_rec_mode() == 2 || !st)
        return;
    memcpy(s_stick, st + 14, sizeof s_stick);
    s_trig[0] = st[12];
    s_trig[1] = st[13];
    s_btn_a = st[6];
    if (s_freecam_freeze)
        memset(st + 4, 0, 18);
}

static float stick(int16_t v)
{
    float f = v / 32767.0f;
    if (f > -0.15f && f < 0.15f)
        return 0.0f;
    return f;
}

/* One frame of flight, from the sticks taken at this poll. */
static void freecam_step(void)
{
    float p = s_cam[3], y = s_cam[4];
    float fx = -sinf(y) * cosf(p), fy = -sinf(p), fz = cosf(y) * cosf(p);
    float rx = cosf(y), rz = sinf(y);
    float speed = s_btn_a > 0x40 ? 12.0f : 3.0f;
    float mf = stick(s_stick[1]) * speed, ms = stick(s_stick[0]) * speed;
    float mu = (s_trig[1] - s_trig[0]) / 255.0f * speed;
    s_cam[0] += fx * mf + rx * ms;
    s_cam[1] += fy * mf + mu;
    s_cam[2] += fz * mf + rz * ms;
    s_cam[4] += stick(s_stick[2]) * 0.05f;
    s_cam[3] -= stick(s_stick[3]) * 0.04f;
    if (s_cam[3] > 1.5f) s_cam[3] = 1.5f;
    if (s_cam[3] < -1.5f) s_cam[3] = -1.5f;
}

/* Around the game's view-matrix build (sub_000292A0, camera in ebx): the
 * view uses the free camera, then the game's camera struct is put back, so
 * the game's own camera logic never sees it. */
static float s_saved[17];
int cheats_view_begin(uint32_t cam)
{
    int k;
    if (!s_freecam)
        return 0;
    if (!s_cam_valid) {             /* start where the game's camera is */
        s_cam[0] = MF(cam + 0); s_cam[1] = MF(cam + 4); s_cam[2] = MF(cam + 8);
        s_cam[3] = MF(cam + 0xC); s_cam[4] = MF(cam + 0x10);
        s_cam_valid = 1;
    }
    for (k = 0; k < 17; k++)
        s_saved[k] = MF(cam + 4 * k);
    MF(cam + 0) = s_cam[0]; MF(cam + 4) = s_cam[1]; MF(cam + 8) = s_cam[2];
    MF(cam + 0xC) = s_cam[3]; MF(cam + 0x10) = s_cam[4];
    MF(cam + 0x14) = 0; MF(cam + 0x18) = 0; MF(cam + 0x1C) = 0;
    MF(cam + 0x38) = 0; MF(cam + 0x3C) = 0; MF(cam + 0x40) = 0;
    return 1;
}

void cheats_view_end(uint32_t cam)
{
    int k;
    for (k = 0; k < 17; k++)
        if (k < 11 || k > 13)          /* +0x2C..+0x34 is the view's own eye */
            MF(cam + 4 * k) = s_saved[k];
}

/* ── The in-game menu's Cheats panel (xbox_ui.h, window thread) ─────────
 *
 * Everything here only reads guest memory or queues a request; the game
 * thread applies the requests. */
static void menu_draw(void *ctx)
{
    static int team = 0, amount = 10000, stage = 0, freeze = 1;
    static const char *const teams[] = { "Time Sweepers", "Tom-Tom Gang" };
    static const char *names[N_STAGES];
    int v;
    (void)ctx;
    if (!names[0]) {
        int i;
        for (i = 0; i < N_STAGES; i++)
            names[i] = s_stages[i].name;
    }
    if (input_rec_mode() == 2) {
        xbox_ui_text("An input replay is running: it applies the cheats it recorded.");
        return;
    }
    if (!cheats_in_game())
        xbox_ui_text("Not in a stage or hub: warp, invincibility and the free camera "
                     "wait until play starts.");

    xbox_ui_separator();
    if (cheats_get_money(0) >= 0)
        xbox_ui_text("Money: Time Sweepers %d G, Tom-Tom Gang %d G",
                     cheats_get_money(0), cheats_get_money(1));
    else
        xbox_ui_text("Money: no save loaded yet");
    xbox_ui_combo("Team", &team, teams, 2);
    xbox_ui_input_int("Amount", &amount);
    if (xbox_ui_button("Give money"))
        cheats_give_money(team, amount);
    xbox_ui_same_line();
    if (xbox_ui_button("Max money"))
        cheats_give_money(team, MONEY_MAX);

    xbox_ui_separator();
    v = cheats_invincible();
    if (xbox_ui_checkbox("Invincible (no damage, HP kept full)", &v))
        cheats_set_invincible(v);
    v = cheats_infinite_ammo();
    if (xbox_ui_checkbox("Infinite ammo (Tom-Tom weapons)", &v))
        cheats_set_infinite_ammo(v);

    xbox_ui_separator();
    v = cheats_freecam_enabled();
    if (xbox_ui_checkbox("Free camera", &v))
        cheats_freecam_enable(v, freeze);
    xbox_ui_same_line();
    if (xbox_ui_checkbox("Freeze the player", &freeze) && cheats_freecam_enabled())
        cheats_freecam_enable(1, freeze);
    xbox_ui_text("Left stick move, right stick look, LT/RT down/up, hold A for speed "
                 "(keyboard: numpad, I/J/K/L, 1/3, Z).");

    xbox_ui_separator();
    {
        static int item = 0;
        static const char *inames[N_ITEMS];
        int own;
        if (!inames[0]) {
            int i;
            for (i = 0; i < N_ITEMS; i++)
                inames[i] = s_items[i].name;
        }
        xbox_ui_combo("Item", &item, inames, N_ITEMS);
        own = cheats_item_owned(team, item);
        xbox_ui_text("%s: %s", teams[team], own < 0 ? "no save loaded"
                     : own ? "owned" : "not owned");
        if (xbox_ui_button("Give item"))
            cheats_give_item(team, item);
        xbox_ui_same_line();
        if (xbox_ui_button("Give every item"))
            cheats_give_all_items(team);
        xbox_ui_text("Items go to the team chosen above (shop and locker).");
    }

    xbox_ui_separator();
    xbox_ui_combo("Stage", &stage, names, N_STAGES);
    if (xbox_ui_button("Warp to stage"))
        cheats_warp_stage(stage);
    xbox_ui_text("Warps with the game's own stage change, as the hub does; "
                 "unsaved progress in the current stage is lost.");
}

void cheats_menu_register(void)
{
    xbox_ui_add_panel("cheats", NULL, menu_draw, NULL);
}

/* ── RECOMP_CHEAT_TRACE: dev tool for finding game state ───────────────
 *
 * RECOMP_CHEAT_TRACE=<va>,<len>,<file>[;<va>,<len>,<file>...] appends, at
 * every pad poll, u32 poll + <len> bytes of guest memory at <va> to <file>.
 * A replay with it shows how a structure changes poll by poll around an
 * event (a hit, a pick-up). Off unless set. */
#define TRACE_MAX 4
static struct { uint32_t va, len; FILE *f; } s_trace[TRACE_MAX];
static int s_trace_n = -1;

static void trace_open(void)
{
    const char *s = getenv("RECOMP_CHEAT_TRACE");
    s_trace_n = 0;
    while (s && *s && s_trace_n < TRACE_MAX) {
        char fn[260];
        char *e;
        const char *c2, *semi;
        size_t n;
        uint32_t va = (uint32_t)strtoul(s, &e, 0), len;
        if (*e != ',')
            break;
        len = (uint32_t)strtoul(e + 1, &e, 0);
        if (*e != ',')
            break;
        c2 = e + 1;
        semi = strchr(c2, ';');
        n = semi ? (size_t)(semi - c2) : strlen(c2);
        if (n >= sizeof fn)
            n = sizeof fn - 1;
        memcpy(fn, c2, n);
        fn[n] = 0;
        s_trace[s_trace_n].va = va;
        s_trace[s_trace_n].len = len;
        s_trace[s_trace_n].f = fopen(fn, "wb");
        if (s_trace[s_trace_n].f)
            fprintf(stderr, "  [CHEAT] tracing 0x%08X+0x%X to %s\n", va, len, fn);
        s_trace_n++;
        s = semi ? semi + 1 : NULL;
    }
}

static void trace_poll(uint32_t poll)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    int k;
    if (s_trace_n < 0)
        trace_open();
    for (k = 0; k < s_trace_n; k++)
        if (s_trace[k].f) {
            fwrite(&poll, 4, 1, s_trace[k].f);
            fwrite(mem + s_trace[k].va, 1, s_trace[k].len, s_trace[k].f);
            if ((poll & 63) == 0)       /* runs are usually killed, not ended */
                fflush(s_trace[k].f);
        }
}

/* ── The poll ── */

static int apply(const Req *r)
{
    int32_t a, b;
    memcpy(&a, r->b + 1, 4);
    memcpy(&b, r->b + 5, 4);
    switch (r->b[0]) {
    case OP_MONEY:
        return apply_money(a, b);
    case OP_INVINCIBLE:
        s_invincible = a != 0;
        fprintf(stderr, "  [CHEAT] invincible %s\n", a ? "on" : "off");
        return 1;
    case OP_WARP:
        return apply_warp(a);
    case OP_FREECAM:
        s_freecam = a != 0;
        s_freecam_freeze = b != 0;
        s_cam_valid = 0;
        fprintf(stderr, "  [CHEAT] free camera %s%s\n", a ? "on" : "off",
                a && b ? " (player frozen)" : "");
        return 1;
    case OP_AMMO:
        s_ammo = a != 0;
        fprintf(stderr, "  [CHEAT] infinite ammo %s\n", a ? "on" : "off");
        return 1;
    case OP_ITEM:
        return apply_item(a, b);
    case OP_CAMPOSE:                /* replay: the recorded free-camera pose */
        memcpy(s_cam, r->b + 1, sizeof s_cam);
        s_cam_valid = 1;
        return 1;
    }
    return 0;
}

void cheats_replay_event(const uint8_t *data)
{
    Req r;
    memcpy(r.b, data, sizeof r.b);
    q_push(&r);
}

/* RECOMP_CHEAT_SCRIPT=<poll>:<cheat>[:a[:b]][;...], for tests: requests at
 * fixed polls of an input replay (poll numbers count only while input is
 * recorded or replayed). Cheats: money:<team>:<amount>, invincible:<0|1>,
 * warp:<list index>, freecam:<0|1>:<freeze>, item:<team>:<list index|-1>,
 * ammo:<0|1>. They bypass the "a replay
 * decides" rule, so do not combine them with a replay of a cheated run. */
static void script_poll(uint32_t poll)
{
    static int n = -1, next;
    static struct { uint32_t poll; Req r; } sc[32];
    if (n < 0) {
        const char *s = getenv("RECOMP_CHEAT_SCRIPT");
        n = 0;
        while (s && *s && n < 32) {
            static const char *const ops[] = { "", "money", "invincible", "warp", "freecam",
                                                "campose", "item", "ammo" };
            char *e;
            int32_t a = 0, b = 0;
            uint8_t op = 0, k;
            sc[n].poll = (uint32_t)strtoul(s, &e, 0);
            if (*e++ != ':')
                break;
            for (k = 1; k < 8; k++)
                if (!strncmp(e, ops[k], strlen(ops[k]))) {
                    op = k;
                    e += strlen(ops[k]);
                }
            if (*e == ':') a = (int32_t)strtol(e + 1, &e, 0);
            if (*e == ':') b = (int32_t)strtol(e + 1, &e, 0);
            memset(&sc[n].r, 0, sizeof sc[n].r);
            sc[n].r.b[0] = op;
            memcpy(sc[n].r.b + 1, &a, 4);
            memcpy(sc[n].r.b + 5, &b, 4);
            if (op)
                n++;
            s = strchr(e, ';');
            if (s)
                s++;
        }
        if (n)
            fprintf(stderr, "  [CHEAT] script: %d requests\n", n);
    }
    while (next < n && sc[next].poll <= poll)
        q_push(&sc[next++].r);
}

void cheats_on_poll(uint32_t poll)
{
    Req r;
    static int tr = -1;
    if (tr < 0)
        tr = getenv("RECOMP_CHEAT_TRACE") != NULL;
    if (tr)
        trace_poll(poll);
    script_poll(poll);
    /* Applied requests go to the recording (or a re-recording), if any, so
     * a replay redoes them at this poll. */
    while (q_pop(&r))
        if (apply(&r))
            input_rec_cheat_event(r.b);
    {   /* Free camera: one step per game frame (the poll runs per pad). */
        static uint32_t last_frame;
        uint32_t frame = M32(0x00AE7180u);
        if (s_freecam && cheats_in_game() && s_cam_valid && input_rec_mode() != 2
                && frame != last_frame) {
            last_frame = frame;
            freecam_step();
            memset(&r, 0, sizeof r);        /* the pose this frame, for replays */
            r.b[0] = OP_CAMPOSE;
            memcpy(r.b + 1, s_cam, sizeof s_cam);
            input_rec_cheat_event(r.b);
        }
    }
    invincible_tick();
    ammo_tick();
}
