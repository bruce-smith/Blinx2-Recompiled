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
extern __declspec(thread) uint32_t g_eax;
extern __declspec(thread) uint32_t g_esp;
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
    now = (long)(clock() * 1000L / CLOCKS_PER_SEC);   /* wall time on MSVC */
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
 * RECOMP_INPUT_RECORD=1 forces it, =0 turns it off. Only changes are
 * written, per pad handle, so a long session of a few thousand presses is a
 * few hundred KB. Each record is flushed: closing the window ends the
 * process without running exit handlers.
 *
 * Replay: RECOMP_INPUT_REPLAY=<file>. The game's call still runs, then its
 * result is replaced by the recording's, so the host pad and keyboard are
 * ignored. Logs [REPLAY] when the last recorded change has been applied, and
 * any desync (a handle at a poll that the recording had another handle at).
 *
 * Format: "BX2INREC", u32 version 1, u32 header length, the header text
 * (build SHAs, start time, save dir), then 40-byte records:
 * u32 poll, u32 vblanks-so-far (diagnostic), u32 handle, u32 result,
 * 22-byte XINPUT_STATE, 2 bytes pad. */
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
static InputRec *s_rp;                         /* replay: all records */
static uint32_t s_rp_n, s_rp_next;
static int      s_rp_ended, s_rp_desync;

static void input_rec_open(void)
{
    const char *rp = getenv("RECOMP_INPUT_REPLAY");
    const char *rc = getenv("RECOMP_INPUT_RECORD");

    s_in_mode = 0;
    if (rp && *rp) {
        FILE *f = fopen(rp, "rb");
        char magic[8];
        uint32_t ver = 0, hlen = 0;
        long size;
        if (!f || fread(magic, 1, 8, f) != 8 || memcmp(magic, "BX2INREC", 8)
            || fread(&ver, 4, 1, f) != 1 || fread(&hlen, 4, 1, f) != 1
            || ver != 1) {
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
        fprintf(stderr, "  [REPLAY] %u recorded changes, last at poll %u; this "
                "build: blinx2-recomp %s, xboxrecomp %s\n", s_rp_n,
                s_rp_n ? s_rp[s_rp_n - 1].poll : 0, BLINX2_GIT_SHA,
                XBOXRECOMP_GIT_SHA);
        fflush(stderr);
        s_in_mode = 2;
        return;
    }
    if (rc ? (*rc == '0') : (getenv("RECOMP_HEADLESS") != NULL))
        return;
    s_in_file = fopen("blinx2_input.rec", "wb");
    if (!s_in_file)
        return;
    {
        char hdr[1024];
        uint32_t ver = 1, hlen;
        time_t now = time(NULL);
        const char *sd = getenv("RECOMP_SAVE_DIR");
        hlen = (uint32_t)snprintf(hdr, sizeof hdr,
            "  blinx2-recomp %s, xboxrecomp %s\n  started %s"
            "  save dir %s (UDATA/TDATA there with RECOMP_SAVE_DIR, else in "
            "the game dir)\n", BLINX2_GIT_SHA, XBOXRECOMP_GIT_SHA,
            ctime(&now), sd && *sd ? sd : "(default)");
        fwrite("BX2INREC", 1, 8, s_in_file);
        fwrite(&ver, 4, 1, s_in_file);
        fwrite(&hlen, 4, 1, s_in_file);
        fwrite(hdr, 1, hlen, s_in_file);
        fflush(s_in_file);
    }
    s_in_mode = 1;
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
    {   /* RECOMP_INPUT_SNAP=<poll>,<file>: write the game image's memory
         * (0x10000-0x1040000: code, .data and BSS) at that poll, for diffing
         * two replays to find where game state first diverges. */
        static int sp = -1;
        static uint32_t snap_poll;
        static char snap_file[260];
        if (sp < 0) {
            const char *s = getenv("RECOMP_INPUT_SNAP");
            const char *c = s ? strchr(s, ',') : NULL;
            sp = 0;
            if (c) {
                snap_poll = (uint32_t)strtoul(s, NULL, 0);
                snprintf(snap_file, sizeof snap_file, "%s", c + 1);
                sp = 1;
            }
        }
        if (sp == 1 && s_in_poll == snap_poll) {
            FILE *f = fopen(snap_file, "wb");
            sp = 2;
            if (f) {
                fwrite(mem + 0x10000, 1, 0x1030000, f);
                fclose(f);
                fprintf(stderr, "  [INPUT] snapshot at poll %u (vblank %u) -> %s\n",
                        s_in_poll, s_vbl_total, snap_file);
                fflush(stderr);
            }
        }
    }
    if (s_in_mode == 0)
        return;
    poll = s_in_poll++;
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
            return;
        /* Apply every record due at this poll. Records are in poll order. */
        while (s_rp_next < s_rp_n && s_rp[s_rp_next].poll <= poll) {
            const InputRec *r = &s_rp[s_rp_next++];
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
        return;
    }

    /* Record: one entry whenever a handle's state or result changes. */
    for (slot = 0; slot < 8; slot++)
        if (s_in_last[slot].handle == handle || !s_in_last[slot].handle)
            break;
    if (slot == 8)
        return;
    /* Compared without dwPacketNumber (the first 4 bytes), which changes on
     * every report; with it, every poll was a record. */
    if (s_in_last[slot].handle == handle && s_in_last[slot].result == g_eax
        && !memcmp(s_in_last[slot].state + 4, mem + state + 4, 18))
        return;
    s_in_last[slot].handle = handle;
    s_in_last[slot].result = g_eax;
    memcpy(s_in_last[slot].state, mem + state, 22);
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

void sub_00368457(void)
{
    static int trace = -1;
    static uint32_t last_handle, last_buttons, calls;
    static int16_t last_sticks[4];
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t handle = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t state  = *(const uint32_t *)(mem + g_esp + 8);

    sub_00368457_gen();
    input_rec_poll(mem, handle, state);

    if (trace < 0)
        trace = getenv("RECOMP_XINPUT_TRACE") != NULL;
    if (trace) {
        uint32_t buttons = *(const uint16_t *)(mem + state + 4)
                         | ((uint32_t)mem[state + 6] << 16);   /* + analog A */
        calls++;
        if (handle != last_handle || buttons != last_buttons
                || (calls & 0x1F) == 1) {
            fprintf(stderr, "  [XINPUT] GetState #%u handle=0x%08X -> %u "
                    "packet=%u buttons=0x%04X A=%u [h+0]=%08X [h+8]=%08X "
                    "[h+A3]=%08X\n", calls, handle, g_eax,
                    *(const uint32_t *)(mem + state),
                    buttons, mem[state + 6],
                    *(const uint32_t *)(mem + handle),
                    *(const uint32_t *)(mem + handle + 8),
                    *(const uint32_t *)(mem + handle + 0xA3));
            last_handle = handle;
            last_buttons = buttons;
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

void sub_00367F0E(void)
{
    static int trace = -1;
    static uint32_t calls;
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t type = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t pins = *(const uint32_t *)(mem + g_esp + 8);
    uint32_t prem = *(const uint32_t *)(mem + g_esp + 12);

    sub_00367F0E_gen();

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
