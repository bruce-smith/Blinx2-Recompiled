/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>         /* getenv */
#include <string.h>         /* memcmp */
#include <time.h>           /* clock */
#if defined(_MSC_VER)
#include <intrin.h>         /* _InterlockedIncrement */

__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);
__declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
#define RECOMP_MANUAL_TLS __declspec(thread)
#else
/* POSIX: the same three from xboxrecomp's platform layer (win32_compat). */
#include "platform/xbox_winnt.h"
#define _InterlockedIncrement(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define RECOMP_MANUAL_TLS __thread
#endif
extern void xbox_SetVblankGate(int (*gate)(int due));
/* A replay of a recorded guest-CPU schedule (xboxrecomp kernel/sched.c)
 * reproduces everything the v2 events below do, and their waits would hold
 * the guest CPU: they stand down for it. */
extern int xbox_SchedReplaying(void);
extern void xbox_VblankKick(void);

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

/* Thread-local in the runtime (RECOMP_TLS), so declared the same way here: a
 * plain extern would compile and then read the wrong storage. */
extern RECOMP_MANUAL_TLS uint32_t g_eax;
extern RECOMP_MANUAL_TLS uint32_t g_esp;
extern ptrdiff_t g_xbox_mem_offset;

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */

/* RECOMP_TIMING_TRACE=1: once a host second, how many vblanks and frames
 * the title saw, and the frame interval it asked for. Sampled around D3D's
 * NV2A ISR (sub_002EA2E0 below), because the seeded generated sub_000604A0
 * runs instead of the callback below, and that runs from the DPC the ISR
 * queues. So this counts ISR calls (raised vblanks); the main loop zeroes
 * the counter at the top of each frame, so a drop in it is a new frame.
 * [0x3FD344] is the vblanks-per-frame target and [0x4AC760] the step
 * the game logic advances by. */
/* Every run also prints one compact [PERF] line a second, so any playtest
 * log is a performance map: frames that second, D3D's NV2A interrupt calls
 * (irq: vblanks plus notifies), the deepest the title's vblank count got
 * before a frame was taken (peak, against its target [0x3FD344]; at the
 * 2-vblank target, 2 means the frame was on time), how much of the second the NV2A
 * executor waited for its raster workers, how much of the workers' time
 * went idle or blocked (barriers, geometry), draw calls and thousands of
 * pixels per frame (many draws: setup-bound; many pixels: raster-bound),
 * and the last asset archive, movie or music file opened -- which scene it
 * was. RECOMP_PERF_LOG=0 turns it off. */
static void perf_line(long ms, uint32_t vbl, uint32_t frames, uint32_t peak,
                      uint32_t target)
{
    extern void nv2a_pb_exec_perf(double *, double *, double *);
    extern void nv2a_pb_exec_counts(uint32_t *, uint64_t *);
    extern const char *xbox_LastAsset(void);
    static int on = -1;
    static long total;
    static uint32_t draws0;
    static uint64_t px0;
    double idle, blocked, wait;
    uint32_t draws, f = frames ? frames : 1;
    uint64_t px;

    if (on < 0) {
        const char *e = getenv("RECOMP_PERF_LOG");
        on = !(e && *e == '0');
    }
    if (!on)
        return;
    total += ms;
    nv2a_pb_exec_perf(&idle, &blocked, &wait);
    nv2a_pb_exec_counts(&draws, &px);
    fprintf(stderr, "[PERF] %lds %u fps %u irq peak %u/%u | executor waited"
            " %.0f%% | workers idle %.0f%% blocked %.0f%% | %u draws %uk px"
            " a frame | %s\n",
            total / 1000, frames, vbl, peak, target, wait * 100, idle * 100,
            blocked * 100, (draws - draws0) / f,
            (unsigned)((px - px0) / f / 1000), xbox_LastAsset());
    draws0 = draws;
    px0 = px;
}

static void timing_trace(uint32_t counter)
{
    static int on = -1;
    static uint32_t vbl, frames, last, peak;
    static long t0;
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    long now;

    if (on < 0)
        on = getenv("RECOMP_TIMING_TRACE") != NULL;
    vbl++;
    if (counter < last)
        frames++;
    if (counter > peak)
        peak = counter;
    last = counter;
#if defined(_MSC_VER)
    now = (long)(clock() * 1000L / CLOCKS_PER_SEC);   /* wall time on MSVC */
#else
    /* POSIX clock() is CPU time summed over every thread, not wall time. */
    now = (long)GetTickCount64();
#endif
    if (!t0)
        t0 = now;
    if (now - t0 >= 1000) {
        /* [0x4B762C]: ADXM's vsync-thread count (sub_002A1B20), one per
         * D3D BlockUntilVerticalBlank; Sofdec times video-only movies by it. */
        static uint32_t adxm_last;
        uint32_t adxm = *(const uint32_t *)(mem + 0x4B762C);
        perf_line(now - t0, vbl, frames, peak,
                  *(const uint32_t *)(mem + 0x3FD344));
        if (on)
            fprintf(stderr, "  [TIMING] %ld ms: %u vblanks, %u frames (peak count %u); "
                "interval [3FD344]=%u step [4AC760]=%u phase [EC4BF8]=%u "
                "cpu [AE71A4]=%u adxm vsyncs +%u usb bw %u/%u %u/%u\n",
                now - t0, vbl, frames, peak,
                *(const uint32_t *)(mem + 0x3FD344),
                *(const uint32_t *)(mem + 0x4AC760),
                *(const uint32_t *)(mem + 0xEC4BF8),
                *(const uint32_t *)(mem + 0xAE71A4), adxm - adxm_last,
                *(const uint16_t *)(mem + 0xF2E0E2), *(const uint16_t *)(mem + 0xF2E0E0),
                *(const uint16_t *)(mem + 0xF2E0E6), *(const uint16_t *)(mem + 0xF2E0E4));
        fflush(stderr);
        adxm_last = adxm;
        vbl = frames = peak = 0;
        t0 = now;
    }
}

/* The title's vertical-blank callback, 0x000604A0:
 *
 *     mov eax, [0x00AE7388]
 *     inc eax
 *     mov [0x00AE7388], eax
 *     ret
 *
 * The main loop paces frames on that counter (sub_0005F960 at 0x00060475:
 * `while (vblanks < target)`). The game hands the address to D3D's
 * SetVerticalBlankCallback (sub_002E0D90), and D3D's NV2A interrupt routine
 * calls it once per vblank. Nothing else references it, so the disassembler
 * never found it and it has no generated body. Every call through it was
 * "[ICALL] Failed to resolve VA 0x000604A0", the counter stayed 0, and the
 * boot hung at the intro movie. Needs RECOMP_VBLANK=1 so the runtime raises
 * the vblank interrupt at all.
 *
 * (Seeding 0x000604A0 into tools/recomp/output/seed_functions.json would make
 * the next regen emit it; this keeps it working without one.) */
static void vblank_callback_000604A0(void)
{
    uint32_t *counter = (uint32_t *)((uintptr_t)g_xbox_mem_offset + 0x00AE7388u);
    (*counter)++;
    g_esp += 4;                     /* ret: pop the return address */
}

/* D3DX's JPEG decoder, guarded by the JPEG SOI marker.
 *
 * D3DX's image-format dispatcher (sub_002F9B7D) tries each decoder in turn
 * until one accepts the data, and this one -- libjpeg -- runs before the
 * decoder that handles some of the title-screen textures (sub_002F8C1C).
 * libjpeg reports a bad header with longjmp back to a setjmp inside this
 * function. Lifted code cannot resume a setjmp site in the middle of a C
 * function: the trace showed it returning with esp 0x90 bytes too high
 * (0x031BF6D8 -> 0x031BF768), after which the dispatcher popped 0xFFFFFFFF
 * into ebx/esi/edi and the caller (sub_002F4EBC) spun forever walking a
 * surface chain from that garbage. The title screen never loaded.
 *
 * On hardware a non-JPEG input costs libjpeg a header check and a longjmp
 * and comes back E_FAIL. Answer E_FAIL for anything without the SOI marker
 * before libjpeg sees it. A genuine JPEG still goes through, and would still
 * hit the same lifting limit if it were corrupt.
 *
 * thiscall: ecx = loader, [esp+4] = source data, [esp+8] = size; ret 8.
 * Requires the recomp to run with --exclude-manual pointing at this file,
 * so the generated body is emitted as sub_002F89C3_gen. */
extern void sub_002F89C3_gen(void);

void sub_002F89C3(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t src  = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t size = *(const uint32_t *)(mem + g_esp + 8);

    if (size < 2 || mem[src] != 0xFF || mem[src + 1] != 0xD8) {
        g_eax = 0x80004005u;            /* E_FAIL, as the other decoders return */
        g_esp += 12;                    /* ret 8 */
        return;
    }
    sub_002F89C3_gen();
}

/* XInputGetState (XAPI, in the XPP section), traced.
 *
 * cdecl (handle, XINPUT_STATE *); copies dwPacketNumber from [handle+8] and
 * the gamepad block from handle+0x14, returns 0 or ERROR_DEVICE_NOT_CONNECTED
 * (0x48F). RECOMP_XINPUT_TRACE=1 logs the first call per handle and every
 * change of packet number + buttons, so "the pad reports Start but the title
 * does not move" can be split into "the title never reads it" and "it reads
 * it and ignores it".
 *
 * Sticks too: a change of either stick is logged with the raw XINPUT axes
 * (state+0x0E..0x14) and the floats the game's pad poll (sub_0021C290) made
 * of pad 0 last time, at 0xEBD28C LX LY RX RY, nominally -1..1. That is how
 * the right stick's Y was caught reading +1..+2 for "down": the poll's
 * `movsx eax, bp` had been lifted as a zero extension. */
extern void sub_00368457_gen(void);

/* ── Input recording and replay ─────────────────────────────────────────
 *
 * Blinx 2 cannot save mid-mission, so a freeze at a mission's end can only
 * be reached again by playing there. This records what the game itself reads
 * -- every XInputGetState's XINPUT_STATE and return value, after the pad,
 * the keyboard merge and XAPI have all had their say -- and plays it back.
 *
 * Keyed on the game's own poll sequence, not on time. The game's logic is
 * frame-locked and polls each opened pad once a frame (sub_0021C290), so
 * "the Nth XInputGetState" is the same moment of game time in two runs that
 * reached it the same way, however fast each ran. Anything that changes how
 * many polls happen before a point (a pad opened at a different poll, a
 * load that takes a different number of frames) shifts everything after it;
 * a replay notices a different handle at a recorded poll and says so.
 *
 * Recording: on by default in normal (non-headless) runs, to
 * blinx2_input.rec next to the exe (rotated like the logs by main.c);
 * RECOMP_INPUT_RECORD=1 forces it, =0 turns it off, =<path> records there
 * (record_blinx2.bat). Only changes are
 * written, per pad handle, so a long session of a few thousand presses is a
 * few hundred KB. Each record is flushed: closing the window ends the
 * process without running exit handlers.
 *
 * Replay: RECOMP_INPUT_REPLAY=<file>. The game's call still runs, then its
 * result is replaced by the recording's, so the host pad and keyboard are
 * ignored. Logs [REPLAY] when the last recorded change has been applied, and
 * any desync (a handle at a poll that the recording had another handle at).
 *
 * Format: "BX2INREC", u32 version 2, u32 header length, the header text
 * (build SHAs, start time, save dir), then 40-byte records:
 * u32 poll, u32 vblanks-so-far (diagnostic), u32 handle, u32 result,
 * 22-byte XINPUT_STATE, 2 bytes pad.
 *
 * Version 2 adds event records, handle IN_EVT: result is the event type,
 * state its value. They carry what the game would otherwise get from host
 * time, so a replay can reproduce it: 1 loading-screen frames, 2/3 vblanks
 * per poll, 4 worker-thread completions, 5 device arrivals (each explained
 * where it is handled, below). Version 1 files (no events) still replay, but
 * drift after the first load or movie. */
#include "cheats.h"
#include "build_sha.h"                 /* written on every build (cmake/git_sha.cmake) */
#ifndef BLINX2_GIT_SHA
#define BLINX2_GIT_SHA "unknown"
#endif
#ifndef XBOXRECOMP_GIT_SHA
#define XBOXRECOMP_GIT_SHA "unknown"
#endif

#pragma pack(push, 1)
typedef struct {
    uint32_t poll, vbl, handle, result;
    uint8_t  state[22];
    uint8_t  pad[2];
} InputRec;
#pragma pack(pop)

static volatile uint32_t s_vbl_total;          /* NV2A ISR calls, bumped below */
static int      s_in_mode = -1;                /* 0 off, 1 record, 2 replay */
static FILE    *s_in_file;
static uint32_t s_in_poll;
static struct { uint32_t handle, result; uint8_t state[22]; } s_in_last[8];
static struct { uint32_t handle, result; uint8_t state[22]; } s_rec_last[8];
static InputRec *s_rp;                         /* replay: all records */
static uint32_t s_rp_n, s_rp_next;
static int      s_rp_ended, s_rp_desync;

#define IN_EVT          0xFFFFFFFFu            /* handle of an event record */
#define IN_EVT_LOADING  1u                     /* value: loading-screen frames */
#define IN_EVT_VBLANK   2u                     /* vblanks before polls, see below */
#define IN_EVT_VBLANK_BIG 3u                   /* value: vblanks before one poll */
#define IN_EVT_DONE     4u                     /* value: index of a "finished" answer */
#define IN_EVT_DEVCHG   5u                     /* call index, type, inserted, removed */
#define IN_EVT_CHEAT    6u                     /* a cheat applied at this poll (cheats.c) */
typedef struct { uint32_t call, type, ins, rem; } DevChg;
static DevChg  *s_ev_dev;                      /* replay: device changes, in order */
static uint32_t s_ev_dev_n;
static uint32_t *s_ev_done, s_ev_done_n;       /* replay: those indexes, in order */
static long     s_main_tid;                    /* the thread that polls the pad */
static uint32_t *s_ev_load;                    /* replay: loading frames, in order */
static uint32_t s_ev_load_n;

/* Vblanks per poll.
 *
 * The game's logic steps a fixed amount a frame, but the vblank interrupt is
 * real time (60 Hz on the host clock) and drives more than the frame pacing:
 * it resumes the ADX server tick (sub_002A17B0 -> sub_002A1B20, ADXM vsync
 * count 0x4B762C), which is the clock Sofdec movies and streamed audio run
 * on. How many vblanks fall between two polls depends on how fast the host
 * ran that frame, so a movie or a voice line lasts a different number of
 * polls from run to run, and every recorded press after it lands at the
 * wrong moment.
 *
 * Recording counts the vblanks raised before each poll: event records of up
 * to 20 one-byte counts (IN_EVT_VBLANK: state[0] = n, state[1..n] the
 * counts for polls r.poll .. r.poll+n-1), or one u32 for a long gap such as
 * a load (IN_EVT_VBLANK_BIG). About 2 bytes a poll, 200 KB an hour.
 *
 * Replay gates the runtime's vblank (xbox_SetVblankGate): between two polls
 * at most the recorded number is raised, on the 60 Hz cadence; a poll that
 * arrives early waits there until the rest have been raised (straight away,
 * not on the cadence). If the game goes 250 ms past the recorded number
 * without polling it gets one anyway, so a different run cannot wedge on a
 * vblank it never receives; those are counted and logged as overruns (loads
 * that took longer here, a stalled host).
 * RECOMP_REPLAY_VBLANK=0 replays without the gate. */
static volatile uint32_t s_vb;                 /* vblanks raised since the gate went in */
static volatile uint32_t s_vb_target = 0xFFFFFFFFu;  /* replay: cap; ~0 = free */
static volatile int      s_vb_hurry;           /* replay: a poll is waiting */
static volatile unsigned long long s_vb_poll_ms;
static volatile uint32_t s_vb_overrun;
static uint32_t *s_vb_at, s_vb_at_n;           /* replay: vblanks at poll k */
static uint32_t s_vb_last, s_vb_buf_poll, s_vb_mismatch;
static uint8_t  s_vb_buf[21];                  /* record: [0] = n, then counts */

static int vblank_gate(int due)
{
    uint32_t t = s_vb_target;
    int go;

    if (t == 0xFFFFFFFFu)
        go = due;
    else if (s_vb < t)
        go = due || s_vb_hurry;
    else if (due && GetTickCount64() - s_vb_poll_ms > 250) {
        go = 1;
        s_vb_overrun++;
    } else
        go = 0;
    if (go)
        s_vb++;
    return go;
}

static void input_rec_open(void)
{
    const char *rp = getenv("RECOMP_INPUT_REPLAY");
    const char *rc = getenv("RECOMP_INPUT_RECORD");

    s_in_mode = 0;
    s_main_tid = (long)GetCurrentThreadId();
    if (rp && *rp) {
        FILE *f = fopen(rp, "rb");
        char magic[8];
        uint32_t ver = 0, hlen = 0;
        long size;
        if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "BX2INREC", 8)
            || fread(&ver, 4, 1, f) != 1 || fread(&hlen, 4, 1, f) != 1
            || ver < 1 || ver > 2) {
            fprintf(stderr, "  [REPLAY] cannot read %s as an input recording\n", rp);
            if (f) fclose(f);
            return;
        }
        {
            char *hdr = (char *)malloc(hlen + 1);
            if (hdr && fread(hdr, 1, hlen, f) == hlen) {
                hdr[hlen] = 0;
                fprintf(stderr, "  [REPLAY] %s, recorded by:\n%s", rp, hdr);
            }
            free(hdr);
        }
        fseek(f, 0, SEEK_END);
        size = ftell(f) - (long)(16 + hlen);
        fseek(f, 16 + (long)hlen, SEEK_SET);
        s_rp_n = size > 0 ? (uint32_t)(size / (long)sizeof(InputRec)) : 0;
        s_rp = (InputRec *)calloc(s_rp_n ? s_rp_n : 1, sizeof(InputRec));
        s_rp_n = (uint32_t)fread(s_rp, sizeof(InputRec), s_rp_n, f);
        fclose(f);
        {
            uint32_t i;
            s_ev_load = (uint32_t *)calloc(s_rp_n ? s_rp_n : 1, sizeof(uint32_t));
            for (i = 0; i < s_rp_n; i++)
                if (s_rp[i].handle == IN_EVT && s_rp[i].result == IN_EVT_LOADING)
                    memcpy(&s_ev_load[s_ev_load_n++], s_rp[i].state, 4);
            s_ev_done = (uint32_t *)calloc(s_rp_n ? s_rp_n : 1, sizeof(uint32_t));
            for (i = 0; i < s_rp_n; i++)
                if (s_rp[i].handle == IN_EVT && s_rp[i].result == IN_EVT_DONE)
                    memcpy(&s_ev_done[s_ev_done_n++], s_rp[i].state, 4);
            s_ev_dev = (DevChg *)calloc(s_rp_n ? s_rp_n : 1, sizeof(DevChg));
            for (i = 0; i < s_rp_n; i++)
                if (s_rp[i].handle == IN_EVT && s_rp[i].result == IN_EVT_DEVCHG)
                    memcpy(&s_ev_dev[s_ev_dev_n++], s_rp[i].state, sizeof(DevChg));
            {   /* RECOMP_REPLAY_THREADS=0: thread completions as they come */
                const char *g = getenv("RECOMP_REPLAY_THREADS");
                if ((g && *g == '0') || xbox_SchedReplaying())
                    s_ev_done_n = 0;
            }
            {   /* RECOMP_REPLAY_LOADING=0: leave the loading screens' lengths free */
                const char *g = getenv("RECOMP_REPLAY_LOADING");
                if ((g && *g == '0') || xbox_SchedReplaying())
                    s_ev_load_n = 0;
            }
            fprintf(stderr, "  [REPLAY] version %u, %u loading screens recorded%s\n",
                    ver, s_ev_load_n, ver < 2 ? " (version 1: loading-screen "
                    "lengths and vblanks not recorded, replay may drift after "
                    "the first load or movie)" : "");
        }
        {
            uint32_t i, top = 0, cum = 0;
            const char *g = getenv("RECOMP_REPLAY_VBLANK");
            for (i = 0; i < s_rp_n; i++)
                if (s_rp[i].handle == IN_EVT && (s_rp[i].result == IN_EVT_VBLANK
                    || s_rp[i].result == IN_EVT_VBLANK_BIG)) {
                    uint32_t end = s_rp[i].poll + (s_rp[i].result == IN_EVT_VBLANK
                                                  ? s_rp[i].state[0] : 1);
                    if (end > top)
                        top = end;
                }
            if (top && !(g && *g == '0') && !xbox_SchedReplaying()) {
                s_vb_at = (uint32_t *)calloc(top, sizeof(uint32_t));
                for (i = 0; i < s_rp_n && s_vb_at; i++) {
                    const InputRec *r = &s_rp[i];
                    uint32_t k, d;
                    if (r->handle != IN_EVT)
                        continue;
                    if (r->result == IN_EVT_VBLANK)
                        for (k = 0; k < r->state[0]; k++) {
                            cum += r->state[1 + k];
                            s_vb_at[r->poll + k] = cum;
                        }
                    else if (r->result == IN_EVT_VBLANK_BIG) {
                        memcpy(&d, r->state, 4);
                        cum += d;
                        s_vb_at[r->poll] = cum;
                    }
                }
                s_vb_at_n = s_vb_at ? top : 0;
                fprintf(stderr, "  [REPLAY] vblanks recorded for %u polls (%u in all); "
                        "gating the vblank to them\n", s_vb_at_n, cum);
                s_vb_target = s_vb_at_n ? s_vb_at[0] : 0xFFFFFFFFu;
                s_vb_poll_ms = GetTickCount64();
                xbox_SetVblankGate(vblank_gate);
            }
        }
        fprintf(stderr, "  [REPLAY] %u recorded changes, last at poll %u; this "
                "build: blinx2-recomp %s, xboxrecomp %s\n", s_rp_n,
                s_rp_n ? s_rp[s_rp_n - 1].poll : 0, BLINX2_GIT_SHA,
                XBOXRECOMP_GIT_SHA);
        fflush(stderr);
        s_in_mode = 2;
        /* RECOMP_INPUT_RERECORD=<file>: record this replay as well -- its
         * input plus this run's events (loading screens, vblanks, thread
         * completions, device arrivals). Turns a version 1 recording into a
         * version 2 one with the same input. */
        rc = getenv("RECOMP_INPUT_RERECORD");
        if (!rc || !*rc) {          /* or RECOMP_INPUT_RECORD=<path> */
            rc = getenv("RECOMP_INPUT_RECORD");
            if (rc && (!rc[0] || !rc[1]))
                rc = NULL;
        }
        if (!rc || !*rc || !(s_in_file = fopen(rc, "wb")))
            return;
        goto header;
    }
    if (rc ? (*rc == '0') : (getenv("RECOMP_HEADLESS") != NULL))
        return;
    s_in_file = fopen(rc && rc[0] && rc[1] ? rc : "blinx2_input.rec", "wb");
    if (!s_in_file)
        return;
header:
    {
        char hdr[1024];
        uint32_t ver = 2, hlen;
        time_t now = time(NULL);
        const char *sd = getenv("RECOMP_SAVE_DIR");
        hlen = (uint32_t)snprintf(hdr, sizeof hdr,
            "  blinx2-recomp %s, xboxrecomp %s\n  started %s"
            "  save dir %s (UDATA/TDATA there with RECOMP_SAVE_DIR, else in "
            "the game dir)\n%s%s%s", BLINX2_GIT_SHA, XBOXRECOMP_GIT_SHA,
            ctime(&now), sd && *sd ? sd : "(default)",
            s_in_mode == 2 ? "  re-recorded from " : "",
            s_in_mode == 2 ? getenv("RECOMP_INPUT_REPLAY") : "",
            s_in_mode == 2 ? "\n" : "");
        fwrite("BX2INREC", 1, 8, s_in_file);
        fwrite(&ver, 4, 1, s_in_file);
        fwrite(&hlen, 4, 1, s_in_file);
        fwrite(hdr, 1, hlen, s_in_file);
        fflush(s_in_file);
    }
    if (s_in_mode != 2)
        s_in_mode = 1;
    xbox_SetVblankGate(vblank_gate);           /* counts; gates only a replay */
}

static void vb_flush(void)
{
    InputRec r;
    if (!s_vb_buf[0])
        return;
    memset(&r, 0, sizeof r);
    r.poll = s_vb_buf_poll;
    r.vbl = s_vbl_total;
    r.handle = IN_EVT;
    r.result = IN_EVT_VBLANK;
    memcpy(r.state, s_vb_buf, 21);
    fwrite(&r, sizeof r, 1, s_in_file);
    fflush(s_in_file);
    s_vb_buf[0] = 0;
}

/* For cheats.c: 0 no recording, 1 recording (live input), 2 replaying. A
 * RECOMP_INPUT_REPLAY_THEN_LIVE replay that has handed over counts as live. */
int input_rec_mode(void)
{
    if (s_in_mode < 0)
        return 0;
    if (s_in_mode == 2 && s_rp_ended && getenv("RECOMP_INPUT_REPLAY_THEN_LIVE"))
        return s_in_file ? 1 : 0;
    return s_in_mode;
}

/* A cheat applied at the current poll, as an event record (IN_EVT_CHEAT,
 * 22-byte payload), so a replay applies it at the same poll. */
void input_rec_cheat_event(const uint8_t *data)
{
    InputRec r;
    if (!s_in_file)
        return;
    vb_flush();                     /* keep the records in poll order */
    memset(&r, 0, sizeof r);
    r.poll = s_in_poll ? s_in_poll - 1 : 0;
    r.vbl = s_vbl_total;
    r.handle = IN_EVT;
    r.result = IN_EVT_CHEAT;
    memcpy(r.state, data, 22);
    fwrite(&r, sizeof r, 1, s_in_file);
    fflush(s_in_file);
}

/* Each poll: record the vblanks raised since the last one, or (replay) make
 * them the recorded number. */
static void vb_poll(uint32_t poll)
{
    if (s_in_file) {
        uint32_t now = s_vb, d = now - s_vb_last;
        s_vb_last = now;
        if (d >= 255) {
            InputRec r;
            vb_flush();
            memset(&r, 0, sizeof r);
            r.poll = poll;
            r.vbl = s_vbl_total;
            r.handle = IN_EVT;
            r.result = IN_EVT_VBLANK_BIG;
            memcpy(r.state, &d, 4);
            fwrite(&r, sizeof r, 1, s_in_file);
            fflush(s_in_file);
        } else {
            if (!s_vb_buf[0])
                s_vb_buf_poll = poll;
            s_vb_buf[1 + s_vb_buf[0]++] = (uint8_t)d;
            if (s_vb_buf[0] == 20)
                vb_flush();
        }
    }
    if (s_in_mode != 2 || !s_vb_at)
        return;
    if (poll < s_vb_at_n) {
        /* Per interval, not cumulative: a run that once needed more vblanks
         * than the recording (a load that took longer and waited on one)
         * would otherwise stay ahead of every later total and be held to
         * the fallback each frame from then on. */
        static uint32_t base;
        uint32_t d = s_vb_at[poll] - (poll ? s_vb_at[poll - 1] : 0);
        uint32_t want = base + d;
        int ms = 0;
        s_vb_target = want;
        while (s_vb < want && ms < 2000) {
            s_vb_hurry = 1;
            xbox_VblankKick();
            Sleep(1);
            ms++;
        }
        s_vb_hurry = 0;
        if (s_vb != want && s_vb_mismatch++ < 20)
            fprintf(stderr, "  [REPLAY] poll %u: %u vblanks since the last, "
                    "recorded %u (%u overruns so far)\n", poll, s_vb - base, d,
                    s_vb_overrun);
        base = s_vb;
        s_vb_target = poll + 1 < s_vb_at_n
            ? base + s_vb_at[poll + 1] - s_vb_at[poll] : 0xFFFFFFFFu;
        if (poll + 1 == s_vb_at_n)
            fprintf(stderr, "  [REPLAY] recorded vblanks used up at poll %u; "
                    "%u overruns, %u polls off\n", poll, s_vb_overrun,
                    s_vb_mismatch);
    }
    s_vb_poll_ms = GetTickCount64();
}

/* After the game's own XInputGetState: record what it got, or replace it. */
static void input_rec_poll(const uint8_t *mem, uint32_t handle, uint32_t state)
{
    uint32_t slot, poll;

    if (s_in_mode < 0)
        input_rec_open();
    {   /* RECOMP_INPUT_TRACE=1: the main loop's mode ([0xAE73F0] & 0x7F) at
         * each change, with the poll count -- for finding where a replay's
         * timing anchors. */
        static int tr = -1;
        static uint32_t last_mode = 0xFFFFFFFFu, n;
        if (tr < 0)
            tr = getenv("RECOMP_INPUT_TRACE") != NULL;
        if (tr) {
            uint32_t mode = *(const uint32_t *)(mem + 0xAE73F0) & 0x7F;
            if (mode != last_mode) {
                fprintf(stderr, "  [INPUT] poll %u vblank %u: mode %u -> %u\n",
                        s_in_poll, s_vbl_total, last_mode, mode);
                last_mode = mode;
            }
            if ((++n % 600) == 0)
                fprintf(stderr, "  [INPUT] poll %u vblank %u\n", s_in_poll, s_vbl_total);
        }
    }
    {   /* RECOMP_INPUT_HASH=<every>,<file>: every <every> polls, a 32-bit
         * hash of each 4 KB page of guest memory 0x10000-0x4000000 (image,
         * game data, heap), appended to <file> as u32 poll, u32 vblank
         * (ISR calls), u32 frame counter [0xAE7180], u32 mode [0xAE73F0],
         * u32 vblanks raised since the recording/replay began, u32 0, then
         * 16368 u32 hashes. Two runs' files show the first poll at which game
         * state differs, and in which pages. */
        static int hp = -1;
        static uint32_t every;
        static FILE *hf;
        if (hp < 0) {
            const char *s = getenv("RECOMP_INPUT_HASH");
            const char *c = s ? strchr(s, ',') : NULL;
            hp = 0;
            if (c && (every = (uint32_t)strtoul(s, NULL, 0)) > 0
                && (hf = fopen(c + 1, "wb")) != NULL)
                hp = 1;
        }
        if (hp == 1 && s_in_poll % every == 0) {
            static uint32_t page_hash[0x3FF0];
            uint32_t pg;
            for (pg = 0; pg < 0x3FF0; pg++) {
                const uint32_t *w = (const uint32_t *)(mem + 0x10000 + pg * 4096u);
                uint32_t h = 2166136261u, i;
                for (i = 0; i < 1024; i++)
                    h = (h ^ w[i]) * 16777619u;
                page_hash[pg] = h;
            }
            {
                uint32_t head[6];
                head[0] = s_in_poll;
                head[1] = s_vbl_total;
                head[2] = *(const uint32_t *)(mem + 0xAE7180);
                head[3] = *(const uint32_t *)(mem + 0xAE73F0);
                head[4] = s_vb;
                head[5] = 0;
                fwrite(head, 4, 6, hf);
            }
            fwrite(page_hash, 4, 0x3FF0, hf);
            fflush(hf);
            {   /* RECOMP_INPUT_HASH_PAGES=<va>[+<va>...]: those 4 KB pages
                 * raw, to <file>.pages, at each checkpoint -- for naming the
                 * dwords behind a page whose hash differs between runs. */
                static int np = -1;
                static uint32_t pages[8];
                static FILE *pf;
                if (np < 0) {
                    const char *e = getenv("RECOMP_INPUT_HASH_PAGES");
                    char *q = (char *)e;
                    np = 0;
                    while (q && *q && np < 8) {
                        pages[np++] = (uint32_t)strtoul(q, &q, 0) & ~0xFFFu;
                        if (*q == '+')
                            q++;
                        else
                            break;
                    }
                    if (np) {
                        char fn[280];
                        snprintf(fn, sizeof fn, "%s.pages",
                                 strchr(getenv("RECOMP_INPUT_HASH"), ',') + 1);
                        pf = fopen(fn, "wb");
                    }
                }
                if (pf) {
                    int k;
                    fwrite(&s_in_poll, 4, 1, pf);
                    for (k = 0; k < np; k++)
                        fwrite(mem + pages[k], 1, 4096, pf);
                    fflush(pf);
                }
            }
        }
    }
    {   /* RECOMP_INPUT_SNAP=<poll>[+<poll>...],<file>: write guest memory
         * 0x10000-0x4000000 (image, game data, heap) at those polls, to
         * <file>.<poll>, for diffing two replays to find where game state
         * first diverges. */
        static int sp = -1, ns;
        static uint32_t snap_poll[16];
        static char snap_file[240];
        if (sp < 0) {
            const char *s = getenv("RECOMP_INPUT_SNAP");
            const char *c = s ? strchr(s, ',') : NULL;
            sp = 0;
            if (c) {
                char *e = (char *)s;
                while (ns < 16 && e < c) {
                    snap_poll[ns++] = (uint32_t)strtoul(e, &e, 0);
                    if (*e == '+')
                        e++;
                    else
                        break;
                }
                snprintf(snap_file, sizeof snap_file, "%s", c + 1);
                sp = 1;
            }
        }
        if (sp == 1) {
            int k;
            for (k = 0; k < ns; k++) {
                char fn[260];
                FILE *f;
                if (s_in_poll != snap_poll[k])
                    continue;
                snprintf(fn, sizeof fn, "%s.%u", snap_file, s_in_poll);
                if ((f = fopen(fn, "wb")) != NULL) {
                    fwrite(mem + 0x10000, 1, 0x3FF0000, f);
                    fclose(f);
                    fprintf(stderr, "  [INPUT] snapshot at poll %u (vblank %u) -> %s\n",
                            s_in_poll, s_vbl_total, fn);
                    fflush(stderr);
                }
            }
        }
    }
    {   /* Changes the in-game menu queued for guest memory (cheats): applied
         * here, on the main thread inside its own turn, never from the
         * window thread (xbox_ui.h). */
        extern void xbox_ui_run_deferred(void);
        xbox_ui_run_deferred();
    }
    if (s_in_mode == 0)
        return;
    poll = s_in_poll++;
    {   /* where a failed schedule replay says it stopped (sched.c) */
        extern volatile uint32_t xbox_sched_poll;
        xbox_sched_poll = poll;
    }
    vb_poll(poll);
    if (!state)
        return;

    if (s_in_mode == 2) {
        /* RECOMP_INPUT_REPLAY_THEN_LIVE=1: once the recording is used up,
         * hand the pad back (the host pad, the keyboard, RECOMP_PAD_LIVE) --
         * replay to a point, then drive by hand or script from there. */
        static int then_live = -1;
        if (then_live < 0)
            then_live = getenv("RECOMP_INPUT_REPLAY_THEN_LIVE") != NULL;
        if (then_live && s_rp_ended)
            goto record;
        /* Apply every record due at this poll. Records are in poll order. */
        while (s_rp_next < s_rp_n && s_rp[s_rp_next].poll <= poll) {
            const InputRec *r = &s_rp[s_rp_next++];
            if (r->handle == IN_EVT) {
                if (r->result == IN_EVT_CHEAT)  /* applied after this poll, as recorded */
                    cheats_replay_event(r->state);
                continue;
            }
            for (slot = 0; slot < 8; slot++)
                if (s_in_last[slot].handle == r->handle || !s_in_last[slot].handle)
                    break;
            if (slot < 8) {
                s_in_last[slot].handle = r->handle;
                s_in_last[slot].result = r->result;
                memcpy(s_in_last[slot].state, r->state, 22);
            }
        }
        for (slot = 0; slot < 8; slot++)
            if (s_in_last[slot].handle == handle)
                break;
        if (slot < 8) {
            /* The gamepad block only. dwPacketNumber stays the live one: XAPI
             * advances it on every USB report, state change or not, so it
             * moves the same way in both runs and carries no input. */
            memcpy((uint8_t *)mem + state + 4, s_in_last[slot].state + 4, 18);
            g_eax = s_in_last[slot].result;
        } else if (s_rp_n && !s_rp_desync) {
            s_rp_desync = 1;
            fprintf(stderr, "  [REPLAY] DESYNC at poll %u (vblank %u): handle "
                    "0x%08X was never recorded by this point\n", poll,
                    s_vbl_total, handle);
            fflush(stderr);
        }
        if (s_rp_next >= s_rp_n && !s_rp_ended) {
            s_rp_ended = 1;
            fprintf(stderr, "  [REPLAY] last recorded change applied at poll %u "
                    "(vblank %u); %s\n", poll, s_vbl_total,
                    getenv("RECOMP_INPUT_REPLAY_THEN_LIVE")
                        ? "live input from the next poll"
                        : "input holds from here");
            fflush(stderr);
        }
        if (!s_in_file)
            return;
    }

record:
    /* A replay handing over to live input records only if asked to: without
     * a file, fwrite(NULL) here froze the process at the handover. */
    if (!s_in_file)
        return;
    /* Record: one entry whenever a handle's state or result changes. */
    for (slot = 0; slot < 8; slot++)
        if (s_rec_last[slot].handle == handle || !s_rec_last[slot].handle)
            break;
    if (slot == 8)
        return;
    /* Compared without dwPacketNumber (the first 4 bytes), which changes on
     * every report; with it, every poll was a record. */
    if (s_rec_last[slot].handle == handle && s_rec_last[slot].result == g_eax
        && !memcmp(s_rec_last[slot].state + 4, mem + state + 4, 18))
        return;
    s_rec_last[slot].handle = handle;
    s_rec_last[slot].result = g_eax;
    memcpy(s_rec_last[slot].state, mem + state, 22);
    {
        InputRec r;
        memset(&r, 0, sizeof r);
        r.poll = poll;
        r.vbl = s_vbl_total;
        r.handle = handle;
        r.result = g_eax;
        memcpy(r.state, mem + state, 22);
        fwrite(&r, sizeof r, 1, s_in_file);
        fflush(s_in_file);
    }
}

/* Loading-screen frames.
 *
 * While the game loads (stage entry, cutscenes, the hub) a thread,
 * sub_0005E410, draws the loading screen: each pass bumps the frame counter
 * [0xAE7180], draws, then Sleep()s, until sub_0005E3C0 sets [0xADCA30] and
 * waits for it to exit. How many passes that is depends on how long the
 * load took in real time, and the frame counter feeds the rest of the game
 * (srand is seeded from it, and it is not polled input, so the poll-keyed
 * replay cannot see it). Measured: the same replay ran 76 and 52 loading
 * frames by poll 1500 of the freeze capture, and diverged from there.
 *
 * Recording: each loading screen's pass count goes in as an event. Replay:
 * the pass count is made the recorded one -- the thread holds in its Sleep
 * on the last recorded pass until the stop flag is set, and the stop waits
 * until the thread has reached that pass. The waits are bounded (120 s, 30 s); a
 * miss is logged as [REPLAY] LOADING. */
extern void sub_0005E410_gen(void);
extern void sub_0005E3C0_gen(void);
extern void sub_00297E0B_gen(void);
static volatile long s_ld_tid;                  /* host thread of the screen */
static volatile long s_ld_passes;               /* Sleeps (= frames) this screen */
static volatile long s_ld_target = -1;          /* replay: recorded passes */
static uint32_t s_ld_screens;                   /* loading screens so far */

void sub_0005E410(void)                         /* the loading-screen thread */
{
    /* The hold goes in from the start, so a load slower than the recorded
     * one cannot run past the recorded count before the stop comes. */
    if (s_in_mode == 2 && s_ld_screens < s_ev_load_n)
        s_ld_target = (long)s_ev_load[s_ld_screens];
    s_ld_tid = (long)GetCurrentThreadId();
    sub_0005E410_gen();                         /* ends in ExitThread */
}

void sub_00297E0B(void)                         /* XAPI Sleep(ms) */
{
    if (s_ld_tid && (long)GetCurrentThreadId() == s_ld_tid) {
        long k = _InterlockedIncrement(&s_ld_passes);
        long t = s_ld_target;
        if (t >= 0 && k >= t) {
            const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
            int ms = 0;
            while (!*(volatile const uint32_t *)(mem + 0xADCA30) && ms < 120000) {
                Sleep(1);
                ms++;
            }
            if (ms >= 120000)
                fprintf(stderr, "  [REPLAY] LOADING screen %u: held at pass %ld "
                        "for 120 s without the stop\n", s_ld_screens, k);
        }
    }
    sub_00297E0B_gen();
}

/* Worker-thread completions.
 *
 * The game copies each stage's archives to the Z: cache on worker threads
 * (sub_001B8BF0) and, once a frame, asks whether each has finished
 * (sub_001B8E70 -> GetExitCodeThread, without waiting). The frame a copy is
 * seen to finish depends on the disk, so the title and menus moved on at a
 * different poll from run to run, and a recorded press then landed on the
 * wrong screen (replays of the freeze capture: two of three stayed on the
 * title past the poll where the recording entered the opening cutscene).
 *
 * Recording: every answer the pad-polling thread gets from GetExitCodeThread
 * is numbered; the numbers of the "finished" ones go in as events. Replay:
 * until the next recorded number the answer is "still running" (STILL_ACTIVE)
 * even if the thread has ended; at it, the call waits for the thread to end
 * for real (up to 60 s) and returns that. Other threads' calls pass through.
 * RECOMP_REPLAY_THREADS=0 turns it off. */
extern void sub_00298004_gen(void);
static uint32_t s_done_calls, s_done_next;

void sub_00298004(void)                         /* XAPI GetExitCodeThread */
{
    uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t esp0 = g_esp;
    uint32_t ret = *(const uint32_t *)(mem + esp0);         /* taken now: the */
    uint32_t h = *(const uint32_t *)(mem + esp0 + 4);       /* callee may use */
    uint32_t pcode = *(const uint32_t *)(mem + esp0 + 8);   /* its arg slots  */
    uint32_t n;
    int ms = 0;

    sub_00298004_gen();
    if (!s_main_tid || (long)GetCurrentThreadId() != s_main_tid || !g_eax || !pcode)
        return;
    n = s_done_calls++;
    if (s_in_mode == 2 && s_done_next < s_ev_done_n) {
        if (s_ev_done[s_done_next] != n) {
            *(uint32_t *)(mem + pcode) = 0x103;          /* STILL_ACTIVE */
            return;
        }
        s_done_next++;
        while (*(const uint32_t *)(mem + pcode) == 0x103 && ms < 60000) {
            Sleep(1);
            ms++;
            g_esp = esp0;                       /* the same call again */
            *(uint32_t *)(mem + esp0) = ret;
            *(uint32_t *)(mem + esp0 + 4) = h;
            *(uint32_t *)(mem + esp0 + 8) = pcode;
            sub_00298004_gen();
        }
        if (ms >= 60000)
            fprintf(stderr, "  [REPLAY] THREAD: answer %u (handle 0x%X) was "
                    "\"finished\" in the recording; still running after 60 s\n",
                    n, h);
    }
    if (s_in_file && *(const uint32_t *)(mem + pcode) != 0x103) {
        InputRec r;
        memset(&r, 0, sizeof r);
        r.poll = s_in_poll;
        r.vbl = s_vbl_total;
        r.handle = IN_EVT;
        r.result = IN_EVT_DONE;
        memcpy(r.state, &n, 4);
        fwrite(&r, sizeof r, 1, s_in_file);
        fflush(s_in_file);
    }
}

void sub_0005E3C0(void)                         /* stop the loading screen */
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t running = *(const uint32_t *)(mem + 0xEC4B94);  /* thread handle */
    uint32_t passes;

    if (s_in_mode < 0)
        input_rec_open();
    if (running && s_in_mode == 2) {
        if (s_ld_screens < s_ev_load_n) {
            /* The caller (sub_000606F0) has already cleared [0xEC4C34], the
             * flag the thread draws a frame on, so a thread still short of
             * the count would spin without drawing. It is set again while
             * the thread catches up, and put back before the real stop. */
            volatile uint32_t *draw = (volatile uint32_t *)(mem + 0xEC4C34);
            uint32_t was = *draw;
            long t = (long)s_ev_load[s_ld_screens];
            int ms = 0;
            s_ld_target = t;
            if (s_ld_passes < t)
                *draw = 1;
            while (s_ld_passes < t && ms < 30000) {
                Sleep(1);
                ms++;
            }
            *draw = was;
            if (ms >= 30000)
                fprintf(stderr, "  [REPLAY] LOADING screen %u: thread at pass %ld "
                        "of %ld after 30 s\n", s_ld_screens, s_ld_passes, t);
        } else if (s_ev_load_n && s_ld_screens == s_ev_load_n)
            fprintf(stderr, "  [REPLAY] LOADING screen %u: past the recorded %u, "
                    "length left to chance\n", s_ld_screens, s_ev_load_n);
    }
    sub_0005E3C0_gen();
    if (!running)
        return;
    passes = (uint32_t)s_ld_passes;
    if (s_in_mode == 2 && s_ld_screens < s_ev_load_n
        && passes != s_ev_load[s_ld_screens])
        fprintf(stderr, "  [REPLAY] LOADING screen %u ran %u frames, recorded %u\n",
                s_ld_screens, passes, s_ev_load[s_ld_screens]);
    if (s_in_file) {
        InputRec r;
        memset(&r, 0, sizeof r);
        r.poll = s_in_poll;
        r.vbl = s_vbl_total;
        r.handle = IN_EVT;
        r.result = IN_EVT_LOADING;
        memcpy(r.state, &passes, 4);
        fwrite(&r, sizeof r, 1, s_in_file);
        fflush(s_in_file);
    }
    {
        static int tr = -1;
        if (tr < 0)
            tr = getenv("RECOMP_INPUT_TRACE") != NULL;
        if (tr)
            fprintf(stderr, "  [INPUT] loading screen %u: %u frames, ended at poll "
                    "%u (frame %u)\n", s_ld_screens, passes, s_in_poll,
                    *(const uint32_t *)(mem + 0xAE7180));
    }
    s_ld_screens++;
    s_ld_passes = 0;
    s_ld_target = -1;
    s_ld_tid = 0;
}

void sub_00368457(void)
{
    static int trace = -1;
    static uint32_t last_handle, last_buttons, calls;
    static int16_t last_sticks[4];
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t handle = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t state  = *(const uint32_t *)(mem + g_esp + 8);

    sub_00368457_gen();
    if (state)
        cheats_pre_poll((uint8_t *)mem + state);
    input_rec_poll(mem, handle, state);
    cheats_on_poll(s_in_poll);

    if (trace < 0)
        trace = getenv("RECOMP_XINPUT_TRACE") != NULL;
    if (trace) {
        uint32_t buttons = *(const uint16_t *)(mem + state + 4)
                         | ((uint32_t)mem[state + 6] << 16);   /* + analog A */
        calls++;
        {   /* any change of the digital and all eight analog buttons */
            static uint8_t last_an[8];
            const uint8_t *an = mem + state + 6;
            if (handle != last_handle || buttons != last_buttons
                    || memcmp(an, last_an, 8) || (calls & 0x1F) == 1) {
                fprintf(stderr, "  [XINPUT] GetState #%u handle=0x%08X -> %u "
                        "packet=%u buttons=0x%04X A=%u "
                        "analog=%u,%u,%u,%u,%u,%u,%u,%u [h+0]=%08X [h+8]=%08X "
                        "[h+A3]=%08X\n", calls, handle, g_eax,
                        *(const uint32_t *)(mem + state),
                        buttons, mem[state + 6], an[0], an[1], an[2], an[3],
                        an[4], an[5], an[6], an[7],
                        *(const uint32_t *)(mem + handle),
                        *(const uint32_t *)(mem + handle + 8),
                        *(const uint32_t *)(mem + handle + 0xA3));
                last_handle = handle;
                last_buttons = buttons;
                memcpy(last_an, an, 8);
            }
        }
        {
            const int16_t *ax = (const int16_t *)(mem + state + 0x0E);
            const float *f = (const float *)(mem + 0x00EBD28Cu);
            if (memcmp(ax, last_sticks, sizeof last_sticks) != 0) {
                fprintf(stderr, "  [XINPUT] sticks L=(%d,%d) R=(%d,%d); game's "
                        "pad 0 last poll L=(%.3f,%.3f) R=(%.3f,%.3f)\n",
                        ax[0], ax[1], ax[2], ax[3], f[0], f[1], f[2], f[3]);
                memcpy(last_sticks, ax, sizeof last_sticks);
            }
        }
    }
}

/* XGetDeviceChanges (XAPI): cdecl (type, DWORD *insertions, DWORD *removals).
 * Under RECOMP_XINPUT_TRACE, log every non-empty change and how often the
 * game's pad poll (sub_0021C290, its only caller here) runs. */
extern void sub_00367F0E_gen(void);

/* Device arrivals (XGetDeviceChanges, called every frame by the main loop).
 *
 * The pad appears when the USB stack has enumerated it, which is host time,
 * so the frame the game first sees it -- and opens it, and starts polling --
 * moved by a few frames from run to run (the frame counter stood at -1..+3
 * against the poll count at the first poll). Recording: the main thread's
 * calls are numbered (from the first one, which also opens the recording),
 * and each that reports a change goes in as an event. Replay: real changes
 * are held back and handed over at the recorded call, waiting (up to 30 s)
 * for the device to really be there; every other call reports none.
 * RECOMP_REPLAY_DEVICES=0 turns it off. */
#define DEV_TYPES 4
static uint32_t s_dev_calls, s_dev_next;
static struct { uint32_t type, ins, rem; } s_dev_pend[DEV_TYPES];

static int dev_pend_slot(uint32_t type)
{
    int i;
    for (i = 0; i < DEV_TYPES; i++)
        if (s_dev_pend[i].type == type || !s_dev_pend[i].type) {
            s_dev_pend[i].type = type;
            return i;
        }
    return 0;
}

void sub_00367F0E(void)
{
    static int trace = -1, devgate = -1;
    static uint32_t calls;
    uint8_t *mem = (uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t esp0 = g_esp;
    uint32_t ret = *(const uint32_t *)(mem + esp0);
    uint32_t type = *(const uint32_t *)(mem + esp0 + 4);
    uint32_t pins = *(const uint32_t *)(mem + esp0 + 8);
    uint32_t prem = *(const uint32_t *)(mem + esp0 + 12);

    if (s_in_mode < 0)
        input_rec_open();
    sub_00367F0E_gen();
    if (devgate < 0) {
        const char *g = getenv("RECOMP_REPLAY_DEVICES");
        devgate = !(g && *g == '0') && !xbox_SchedReplaying();
    }
    if (s_main_tid && (long)GetCurrentThreadId() == s_main_tid) {
        uint32_t n = s_dev_calls++;
        if (s_in_mode == 2 && devgate && s_dev_next < s_ev_dev_n) {
            const DevChg *e = &s_ev_dev[s_dev_next];
            int k = dev_pend_slot(type);
            int ms = 0;
            s_dev_pend[k].ins |= *(uint32_t *)(mem + pins);
            s_dev_pend[k].rem |= *(uint32_t *)(mem + prem);
            if (e->call == n && e->type == type) {
                s_dev_next++;
                while (((s_dev_pend[k].ins & e->ins) != e->ins
                        || (s_dev_pend[k].rem & e->rem) != e->rem) && ms < 30000) {
                    Sleep(1);
                    ms++;
                    g_esp = esp0;               /* ask again */
                    *(uint32_t *)(mem + esp0) = ret;
                    *(uint32_t *)(mem + esp0 + 4) = type;
                    *(uint32_t *)(mem + esp0 + 8) = pins;
                    *(uint32_t *)(mem + esp0 + 12) = prem;
                    sub_00367F0E_gen();
                    s_dev_pend[k].ins |= *(uint32_t *)(mem + pins);
                    s_dev_pend[k].rem |= *(uint32_t *)(mem + prem);
                }
                if (ms >= 30000)
                    fprintf(stderr, "  [REPLAY] DEVICE: change %u (insert 0x%X remove "
                            "0x%X) not seen after 30 s\n", n, e->ins, e->rem);
                *(uint32_t *)(mem + pins) = e->ins;
                *(uint32_t *)(mem + prem) = e->rem;
                s_dev_pend[k].ins &= ~e->ins;
                s_dev_pend[k].rem &= ~e->rem;
                g_eax = 1;
            } else {
                *(uint32_t *)(mem + pins) = 0;
                *(uint32_t *)(mem + prem) = 0;
                g_eax = 0;
            }
        }
        if (s_in_file && (*(uint32_t *)(mem + pins) || *(uint32_t *)(mem + prem))) {
            InputRec r;
            DevChg c;
            c.call = n;
            c.type = type;
            c.ins = *(uint32_t *)(mem + pins);
            c.rem = *(uint32_t *)(mem + prem);
            memset(&r, 0, sizeof r);
            r.poll = s_in_poll;
            r.vbl = s_vbl_total;
            r.handle = IN_EVT;
            r.result = IN_EVT_DEVCHG;
            memcpy(r.state, &c, sizeof c);
            fwrite(&r, sizeof r, 1, s_in_file);
            fflush(s_in_file);
        }
    }

    if (trace < 0)
        trace = getenv("RECOMP_XINPUT_TRACE") != NULL;
    if (trace) {
        uint32_t ins = *(const uint32_t *)(mem + pins);
        uint32_t rem = *(const uint32_t *)(mem + prem);
        calls++;
        if (ins || rem || (calls & 0x1F) == 1)
            fprintf(stderr, "  [XINPUT] DeviceChanges #%u type=0x%08X -> %u "
                    "insert=0x%X remove=0x%X\n", calls, type, g_eax, ins, rem);
    }
}

/* D3D's NV2A interrupt routine (sub_002EA2E0), sampled. RECOMP_VBL_TRACE=1
 * prints the fields it gates the vblank work on, every 60th call. It showed
 * PMC_INTR_0 already zero when the ISR ran (the ack thread cleared it), which
 * is why the frame callback ran at 2-3 Hz. */
extern void sub_002EA2E0_gen(void);

void sub_002EA2E0(void)
{
    static int trace = -1;
    static uint32_t calls;
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t esi_ = *(const uint32_t *)(mem + g_esp + 8);

    if (trace < 0)
        trace = getenv("RECOMP_VBL_TRACE") != NULL;
    if (trace && (calls++ % 60) == 0 && esi_) {
        uint32_t edi_ = *(const uint32_t *)(mem + esi_);
#define F(o) (*(const uint32_t *)(mem + esi_ + (o)))
        fprintf(stderr, "  [VBL] #%u esi=%08X A0=%08X A4=%08X 8=%08X 1B4=%08X "
                "1BC=%08X 1C0=%08X 1D0=%08X edi=%08X [edi+140]=%08X "
                "[edi+100]=%08X\n", calls, esi_, F(0xA0), F(0xA4), F(8),
                F(0x1B4), F(0x1BC), F(0x1C0), F(0x1D0), edi_,
                edi_ ? *(const uint32_t *)(mem + edi_ + 0x140) : 0,
                edi_ ? *(const uint32_t *)(mem + edi_ + 0x100) : 0);
#undef F
    }
    {
        const uint32_t *vbl = (const uint32_t *)(mem + 0x00AE7388u);
        s_vbl_total++;
        timing_trace(*vbl);
        sub_002EA2E0_gen();
    }
}

/* D3D's PGRAPH interrupt handler (sub_002EAA50), run from its NV2A DPC
 * (sub_002EAC90). It acknowledges PGRAPH_INTR (0xFD400100) the hardware way,
 * by writing back the bits it saw -- write-1-to-clear -- and returns the
 * register's value after that; the DPC calls it again while that is non-zero.
 * The aperture here is plain RAM, so the write cleared nothing and the DPC
 * re-ran the handler, and the notify it dispatches, until something else
 * zeroed the register. Applying the write-1-to-clear after the generated body
 * gives the DPC the value it would read on the console. Needed for the
 * PGRAPH NOTIFY the executor raises (xbox_Nv2aNotify): Blinx 2's fence wait
 * sleeps on one when the GPU is far behind. */
extern void sub_002EAA50_gen(void);

void sub_002EAA50(void)
{
    volatile uint32_t *intr =
        (volatile uint32_t *)(uintptr_t)(g_xbox_mem_offset + 0xFD400100u);
    uint32_t seen = *intr;

    sub_002EAA50_gen();
    *intr &= ~seen;
    g_eax = *intr;
}

/* The collision octree's subdivision, guarded against its own pools.
 *
 * sub_001645A0(node) splits a node holding more than 50 triangles into 8
 * children taken from a fixed pool of 2000 nodes at 0xCAE8A8 (count
 * 0xCC9E30) and links each triangle into every child it overlaps, from a
 * pool of 60000 links at 0xBACB98 (count 0xCCA048). Neither pool is bounds
 * checked, and node 2000 is the node counter itself, so an overflow rewrites
 * the counter and the tree's pointers and the next walk crashes (P3 saw it
 * in sub_001645D0). Stage 01-01 alone peaks at 1161/2000 nodes and
 * 46107/60000 links, so a larger stage can overflow with valid input.
 *
 * When the 8 children, or up to 8 links for each of the node's triangles,
 * would not fit, the node is left a leaf: collision queries over it test
 * more triangles, and are still correct. This changes nothing until the game
 * would otherwise corrupt memory. Logged once, with the counts.
 *
 * cdecl (node); its own recursion calls this symbol too, so every level is
 * guarded. Needs the recomp run with --exclude-manual (emits
 * sub_001645A0_gen). */
extern void sub_001645A0_gen(void);

void sub_001645A0(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t node  = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t count = *(const uint32_t *)(mem + node + 0x24);

    {   /* How close each build came: the counters restart from 0 for every
         * build (sub_001642A0), so a drop means the previous build is done.
         * One line per build, printed when the next one starts. */
        static uint32_t peak_n, peak_l, last_n, builds;
        uint32_t n = *(const uint32_t *)(mem + 0xCC9E30);
        uint32_t l = *(const uint32_t *)(mem + 0xCCA048);
        if (n < last_n && peak_n) {
            fprintf(stderr, "  [OCTREE] build %u peaked at %u/2000 nodes, "
                    "%u/60000 links\n", ++builds, peak_n, peak_l);
            peak_n = peak_l = 0;
        }
        if (n > peak_n) peak_n = n;
        if (l > peak_l) peak_l = l;
        last_n = n;
    }

    if ((int32_t)count > 50) {
        /* RECOMP_OCTREE_TEST_NODES / _LINKS lower the limits, to exercise
         * this path on a stage that fits. */
        static uint32_t max_n, max_l;
        uint32_t nodes = *(const uint32_t *)(mem + 0xCC9E30);
        uint32_t links = *(const uint32_t *)(mem + 0xCCA048);
        if (!max_n) {
            const char *a = getenv("RECOMP_OCTREE_TEST_NODES");
            const char *b = getenv("RECOMP_OCTREE_TEST_LINKS");
            max_n = (a && atoi(a) > 8 && atoi(a) < 2000) ? (uint32_t)atoi(a) : 2000;
            max_l = (b && atoi(b) > 0 && atoi(b) < 60000) ? (uint32_t)atoi(b) : 60000;
        }
        if (nodes + 8 > max_n || (uint64_t)links + (uint64_t)count * 8 > max_l) {
            static int warned;                  /* main thread only */
            if (!warned++) {
                fprintf(stderr, "  [OCTREE] pool full: not splitting node 0x%08X "
                        "(%u triangles); nodes %u/2000, links %u/60000 -- it "
                        "stays a leaf (logged once)\n", node, count, nodes, links);
                fflush(stderr);
            }
            g_esp += 4;                         /* ret */
            return;
        }
    }
    sub_001645A0_gen();
}

/* ── Cheat hooks (cheats.c) ────────────────────────────────────────────
 *
 * The game's hit routine, cdecl (victim actor, attacker, 6 more dwords):
 * subtracts the damage from the victim's HP (+0x6C), knocks it back and
 * spawns the hit effects. With invincibility on, a human-controlled victim
 * is not hit at all. Off, it is the generated body unchanged. */
extern void sub_0019B790_gen(void);

void sub_0019B790(void)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    if (cheats_block_damage(*(const uint32_t *)(mem + g_esp + 4))) {
        g_eax = 0;
        g_esp += 4;                 /* ret; the caller pops the arguments */
        return;
    }
    sub_0019B790_gen();
}

/* The view-matrix build, camera struct in ebx (identity, the struct's
 * angles, then its eye; into 0xA72380, D3D's view, and the inverse at
 * 0xA9C220). The free camera substitutes its pose for this one build and
 * puts the game's camera back afterwards. */
extern RECOMP_MANUAL_TLS uint32_t g_ebx;
extern void sub_000292A0_gen(void);

void sub_000292A0(void)
{
    uint32_t cam = g_ebx;
    int fc = cheats_view_begin(cam);
    sub_000292A0_gen();
    if (fc)
        cheats_view_end(cam);
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x000604A0u)
        return vblank_callback_000604A0;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
