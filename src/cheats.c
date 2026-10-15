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
#include <intrin.h>

#include "cheats.h"

extern ptrdiff_t g_xbox_mem_offset;
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __declspec(thread) uint32_t g_ebx, g_esi, g_edi, g_ebp;
extern __declspec(thread) double g_fp_stack[8];
extern __declspec(thread) int g_fp_top;

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

enum { OP_NONE, OP_MONEY, OP_INVINCIBLE, OP_WARP, OP_FREECAM, OP_CAMPOSE };

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

static volatile int s_invincible;
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
 * warp:<list index>, freecam:<0|1>:<freeze>. They bypass the "a replay
 * decides" rule, so do not combine them with a replay of a cheated run. */
static void script_poll(uint32_t poll)
{
    static int n = -1, next;
    static struct { uint32_t poll; Req r; } sc[32];
    if (n < 0) {
        const char *s = getenv("RECOMP_CHEAT_SCRIPT");
        n = 0;
        while (s && *s && n < 32) {
            static const char *const ops[] = { "", "money", "invincible", "warp", "freecam" };
            char *e;
            int32_t a = 0, b = 0;
            uint8_t op = 0, k;
            sc[n].poll = (uint32_t)strtoul(s, &e, 0);
            if (*e++ != ':')
                break;
            for (k = 1; k < 5; k++)
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
}
