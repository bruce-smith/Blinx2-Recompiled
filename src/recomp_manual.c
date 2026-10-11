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
 * it and ignores it". */
extern void sub_00368457_gen(void);

void sub_00368457(void)
{
    static int trace = -1;
    static uint32_t last_handle, last_buttons, calls;
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t handle = *(const uint32_t *)(mem + g_esp + 4);
    uint32_t state  = *(const uint32_t *)(mem + g_esp + 8);

    sub_00368457_gen();

    if (trace < 0)
        trace = getenv("RECOMP_XINPUT_TRACE") != NULL;
    if (trace) {
        uint32_t buttons = *(const uint16_t *)(mem + state + 4);
        calls++;
        if (handle != last_handle || buttons != last_buttons
                || (calls & 0x1F) == 1) {
            fprintf(stderr, "  [XINPUT] GetState #%u handle=0x%08X -> %u "
                    "packet=%u buttons=0x%04X A=%u [h+0]=%08X [h+8]=%08X "
                    "[h+A3]=%08X\n", calls, handle, g_eax,
                    *(const uint32_t *)(mem + state),
                    buttons, mem[state + 8],
                    *(const uint32_t *)(mem + handle),
                    *(const uint32_t *)(mem + handle + 8),
                    *(const uint32_t *)(mem + handle + 0xA3));
            last_handle = handle;
            last_buttons = buttons;
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
