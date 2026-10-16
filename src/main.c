/**
 * Blinx 2 - Recompiled Game Entry Point
 *
 * This is the Windows executable that hosts the recompiled game code.
 * It performs the following initialization sequence:
 *
 * 1. Load the original XBE file from disk
 * 2. Initialize the Xbox memory layout (map data sections to original VAs)
 * 3. Initialize the Xbox kernel replacement layer
 * 4. Initialize the kernel bridge (thunk table in Xbox memory)
 * 5. Set up game file paths for I/O redirection
 * 6. Initialize the stack pointer
 * 7. Install VEH crash handler for diagnostics
 * 8. Call the game's original entry point (recompiled)
 *
 * Customize this file for your game:
 *   - Set BLINX_2_ENTRY_POINT to the XBE entry point address
 *   - The game files (default.xbe and its archives) are found by
 *     resolve_game_dir(): RECOMP_GAME_DIR, else game_files\ next to the exe,
 *     else the CMake option BLINX2_GAME_DIR
 *   - Add any CRT global pre-initialization your game needs
 *   - Customize the VEH handler for game-specific crash diagnosis
 *
 * XBE Details (fill in from xbe_parser output):
 *   Title:       BLiNX2
 *   Title ID:    0x4D530065
 *   Base addr:   0x00010000
 *   Entry point: 0x0029A1DA
 *   Code size:   50506784 KB (.text)
 *   Sections:    ?? (list them)
 *   Kernel imports: ??
 */

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>       /* timeBeginPeriod */
#include <dbghelp.h>
#include <io.h>
#else
/* POSIX: the Win32 names below come from xboxrecomp's platform layer
 * (win32_compat), reached through xboxrecomp.h. */
#include <unistd.h>
#include <glob.h>
#include <time.h>
#include <sys/stat.h>
#define _snprintf snprintf
#define _putenv_s(name, value) setenv((name), (value), 1)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <math.h>

/* xboxrecomp runtime headers */
#include <xbox/xboxrecomp.h>
#include "ohci.h"          /* xbox_usb: USB host controller model */
#include "xbox_ui.h"        /* settings file, in-game menu */
#include "netplay.h"        /* lockstep network play */
#include "cheats.h"         /* the menu's Cheats tab */
#include "build_sha.h"      /* for the menu's About text (cmake/git_sha.cmake) */
#ifndef BLINX2_GIT_SHA
#define BLINX2_GIT_SHA "unknown"
#endif
#ifndef XBOXRECOMP_GIT_SHA
#define XBOXRECOMP_GIT_SHA "unknown"
#endif
#ifndef RECOMP_GEN_HASH
#define RECOMP_GEN_HASH "none"
#endif

/* The guest-CPU schedule (xboxrecomp kernel/sched.c). */
extern int  xbox_SchedMode(void);
extern void xbox_SchedAddTick(void (*fn)(void));
extern void xbox_SchedAddPump(void (*fn)(void));
extern void xbox_SchedFlush(void);

/*
 * If xboxrecomp.h is not an umbrella header in your setup, include
 * the individual headers directly:
 *
 * #include "kernel.h"
 * #include "xbox_memory_layout.h"
 * #include "d3d8_xbox.h"
 * #include "dsound_xbox.h"
 * #include "xinput_xbox.h"
 */

/* ── Global register state (defined in xbox_memory_layout.c) ── */

/* RECOMP_TLS is not optional here. The runtime defines these thread-local, and
 * a plain `extern` referencing a __declspec(thread) variable does not resolve
 * to the calling thread's copy -- it resolves to the image's TLS template. The
 * host side then writes g_esp somewhere the generated code never reads, so the
 * guest starts with every register at zero and faults immediately, having
 * apparently ignored the setup that visibly ran. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
/* x87 and SSE state. Global for the same reason the volatile GPRs are: one
 * guest routine can lift to several C functions, so a value written in one
 * body is read in the next. Defined in xbox_memory_layout.c like the rest of
 * the register file. */
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS int g_fp_top;
extern RECOMP_TLS uint16_t g_fp_control_word;
extern RECOMP_TLS int g_fp_cmp;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;
extern ptrdiff_t g_xbox_mem_offset;

/* ── XBE Constants ─────────────────────────────────────────── */

/*
 * TODO: Set these from your xbe_parser output.
 * Run: py -3 -m tools.xbe_parser game/default.xbe
 */
#define BLINX_2_ENTRY_POINT   0x0029A1DA  /* XBE entry point VA */
#ifndef BLINX2_GAME_DIR_DEFAULT
#define BLINX2_GAME_DIR_DEFAULT "E:\\Blinx2project\\game_files"
#endif

/* The directory of the extracted game files: default.xbe and the .ipk/.iph
 * archives, as unpacked from the disc image. In order:
 *   1. RECOMP_GAME_DIR, if set;
 *   2. game_dir= under [paths] in blinx2_settings.ini, if set;
 *   3. game_files\ next to the exe, if it holds default.xbe;
 *   4. BLINX2_GAME_DIR, set when configuring with CMake
 *      (-DBLINX2_GAME_DIR=...; default E:/Blinx2project/game_files).
 * Saves (UDATA/TDATA) live in this directory unless RECOMP_SAVE_DIR moves
 * them. Called once the working directory is the exe's own folder. */
static char g_game_dir[MAX_PATH];
static char g_xbe_path[MAX_PATH];
static int s_headless;

/* game_dir= under [paths] in blinx2_settings.ini. A headless run does not
 * load the settings (the other options would change what a scripted run
 * does), so it reads this one key from the file itself: a packaged build
 * pointed at its game files then runs headless too. */
static void settings_game_dir(char *out, size_t n)
{
    FILE *f;
    char line[512], section[32] = "";
    out[0] = 0;
    if (!s_headless) {
        xbox_setting_get_str("paths", "game_dir", "", out, n);
        return;
    }
    if (!(f = fopen("blinx2_settings.ini", "r")))
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = line, *e;
        while (*p == ' ' || *p == '\t')
            p++;
        for (e = p + strlen(p); e > p && (e[-1] == '\n' || e[-1] == '\r'
                                          || e[-1] == ' ' || e[-1] == '\t'); )
            *--e = 0;
        if (*p == '[') {
            snprintf(section, sizeof section, "%.*s", (int)strcspn(p + 1, "]"), p + 1);
        } else if (!strcmp(section, "paths") && !strncmp(p, "game_dir", 8)) {
            p += 8;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p++ != '=')
                continue;
            while (*p == ' ' || *p == '\t')
                p++;
            snprintf(out, n, "%s", p);
            break;
        }
    }
    fclose(f);
}

#if !defined(_WIN32)
/* The executable's folder; the working directory may be elsewhere (see the
 * read-only case in WinMain). */
static char g_exe_dir[4096] = ".";

static void resolve_game_dir(void)
{
    const char *env = getenv("RECOMP_GAME_DIR");
    char next_to_exe[4096 + 32], ini[4096];
    size_t n;

    settings_game_dir(ini, sizeof ini);
    if (!(env && *env) && ini[0])
        env = ini;

    snprintf(next_to_exe, sizeof next_to_exe, "%s/game_files", g_exe_dir);
    if (env && *env)
        snprintf(g_game_dir, sizeof g_game_dir, "%s", env);
    else if (snprintf(g_xbe_path, sizeof g_xbe_path, "%s/default.xbe", next_to_exe),
             access(g_xbe_path, R_OK) != 0 || !realpath(next_to_exe, g_game_dir))
        snprintf(g_game_dir, sizeof g_game_dir, "%s", BLINX2_GAME_DIR_DEFAULT);
    n = strlen(g_game_dir);
    while (n > 1 && g_game_dir[n - 1] == '/')
        g_game_dir[--n] = 0;
    snprintf(g_xbe_path, sizeof g_xbe_path, "%s/default.xbe", g_game_dir);
}
#else
static void resolve_game_dir(void)
{
    const char *env = getenv("RECOMP_GAME_DIR");
    char *p, ini[MAX_PATH];
    size_t n;

    settings_game_dir(ini, sizeof ini);
    if (!(env && *env) && ini[0])
        env = ini;

    if (env && *env) {
        snprintf(g_game_dir, sizeof g_game_dir, "%s", env);
    } else if (GetFileAttributesA("game_files\\default.xbe") != INVALID_FILE_ATTRIBUTES
               && GetFullPathNameA("game_files", sizeof g_game_dir, g_game_dir, NULL)) {
        /* game_files next to the exe */
    } else {
        snprintf(g_game_dir, sizeof g_game_dir, "%s", BLINX2_GAME_DIR_DEFAULT);
    }
    for (p = g_game_dir; *p; p++)
        if (*p == '/')
            *p = '\\';
    n = strlen(g_game_dir);
    while (n > 3 && g_game_dir[n - 1] == '\\')
        g_game_dir[--n] = 0;
    snprintf(g_xbe_path, sizeof g_xbe_path, "%s\\default.xbe", g_game_dir);
}
#endif
#define BLINX_2_XBE_PATH      g_xbe_path
#define BLINX_2_DIR           g_game_dir

/* ── Forward declarations ──────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size);

/* Recompiled entry point (generated by recomp pipeline) */
extern void xbe_entry_point(void);
/* The flat dispatch table (recomp_dispatch.c); declared in recomp_types.h,
 * which main.c does not include. gcc 15 refuses the implicit declaration. */
extern int recomp_dispatch_init(void);

/* ── VEH crash handler ─────────────────────────────────────── */

/*
 * Vectored Exception Handler for crash diagnostics.
 *
 * When the recompiled game hits an access violation, this handler prints
 * the faulting address, all Xbox register values, and a native stack trace.
 * This is your primary debugging tool during bring-up.
 *
 * Customize this for your game:
 *   - Add game-specific address checks (GPU register probes, etc.)
 *   - Add dumps of game-specific globals (heap handles, state flags)
 *   - Add SEH simulation if your game uses __try/__except
 */
/* Name the guest function a fault happened in, and recover the call chain.
 *
 * Recompiled code faults as ordinary native code, so the exception record
 * carries a host RIP and nothing else -- there is no guest program counter to
 * report, and the host address changes every build. Two things recover the
 * guest view:
 *
 *   - every generated function is a real symbol in the image (sub_005A03C0
 *     and so on), so the linker's PDB already maps host address back to guest
 *     function. dbghelp turns an anonymous RIP into that name.
 *
 *   - every lifted call pushes its guest return address onto the guest stack
 *     before jumping, so the stack still holds the chain. Scanning up from esp
 *     for values inside the code sections recovers it.
 *
 * ponytail: the stack scan is a scan, not a frame walk -- these are FPO frames
 * with no reliable ebp chain, so there is nothing to walk. It over-reports,
 * since addresses from returned-from calls linger below esp, but naming the
 * guest function is the whole question at a fault.
 *
 * Requires linking dbghelp and keeping the .pdb beside the .exe.
 */
/* RECOMP_SAMPLE_MS=<n>: host-RIP sampler on the guest main thread.
 *
 * Every n ms the main thread is suspended, its host stack unwound (x64 unwind
 * data needs no PDB) and each frame's function counted: "self" for the
 * innermost frame, "incl" once per sample for every frame. A report of both,
 * named through the PDB (link with /DEBUG), goes to stderr every 30 s. This is
 * how a compute-bound stretch (the boot's .rec scan) is attributed to guest
 * functions without regenerating with --trace-functions.
 *
 * Nothing that can take a lock runs while the target is suspended: no stdio,
 * no heap -- only the unwinder and a static table. */
#define SAMPLE_SLOTS 8192
static struct { DWORD64 fn; unsigned long self, incl; } s_samples[SAMPLE_SLOTS];
static HANDLE s_sample_target;

static unsigned long *sample_slot(DWORD64 fn, int self)
{
    unsigned i = (unsigned)((fn >> 4) * 2654435761u) & (SAMPLE_SLOTS - 1), n;
    for (n = 0; n < SAMPLE_SLOTS; n++, i = (i + 1) & (SAMPLE_SLOTS - 1)) {
        if (s_samples[i].fn == fn || !s_samples[i].fn) {
            s_samples[i].fn = fn;
            return self ? &s_samples[i].self : &s_samples[i].incl;
        }
    }
    return NULL;
}

static void sample_report(unsigned long total)
{
    static int order[SAMPLE_SLOTS];
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    int pass, n = 0, i, j;

    for (i = 0; i < SAMPLE_SLOTS; i++)
        if (s_samples[i].fn) order[n++] = i;
    for (pass = 0; pass < 2; pass++) {
        fprintf(stderr, "[SAMPLE] %lu samples, top by %s:\n", total,
                pass ? "inclusive" : "self");
        for (i = 0; i < 30 && i < n; i++) {          /* partial selection sort */
            int best = i;
            for (j = i + 1; j < n; j++) {
                unsigned long a = pass ? s_samples[order[j]].incl : s_samples[order[j]].self;
                unsigned long b = pass ? s_samples[order[best]].incl : s_samples[order[best]].self;
                if (a > b) best = j;
            }
            { int t = order[i]; order[i] = order[best]; order[best] = t; }
            {
                DWORD64 disp = 0, fn = s_samples[order[i]].fn;
                unsigned long c = pass ? s_samples[order[i]].incl : s_samples[order[i]].self;
                if (!c) break;
                memset(buf, 0, sizeof(buf));
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                fprintf(stderr, "  %8lu %5.1f%%  %s\n", c, 100.0 * c / total,
                        SymFromAddr(GetCurrentProcess(), fn, &disp, sym)
                            ? sym->Name : "?");
            }
        }
    }
    fflush(stderr);
}

static DWORD WINAPI sampler_main(LPVOID param)
{
    DWORD ms = (DWORD)(uintptr_t)param, last = GetTickCount();
    unsigned long total = 0;

    for (;;) {
        CONTEXT ctx;
        DWORD64 seen[16];
        int nseen = 0, d, k;

        Sleep(ms);
        if (SuspendThread(s_sample_target) == (DWORD)-1)
            return 0;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(s_sample_target, &ctx)) {
#if defined(_WIN32)
            for (d = 0; d < 16 && ctx.Rip; d++) {
                DWORD64 base = 0, fn;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                unsigned long *c;
                if (!rf) {                  /* leaf: return address at [rsp] */
                    fn = ctx.Rip;
                    ctx.Rip = *(DWORD64 *)ctx.Rsp;
                    ctx.Rsp += 8;
                } else {
                    void *hd; DWORD64 ef;
                    fn = base + rf->BeginAddress;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf,
                                     &ctx, &hd, &ef, NULL);
                }
#else
            /* POSIX: no unwinder that can walk another thread's stack here,
             * so the innermost frame only. It is keyed by its function
             * (dladdr), looked up once the thread runs again: nothing that
             * can take a lock runs while it is stopped. */
            ResumeThread(s_sample_target);
            for (d = 0; d < 1 && ctx.Rip; d++) {
                DWORD64 fn = ctx.Rip;
                unsigned long *c;
                {
                    char sb[sizeof(SYMBOL_INFO) + 64];
                    SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
                    DWORD64 disp = 0;
                    memset(sb, 0, sizeof sb);
                    si->SizeOfStruct = sizeof(SYMBOL_INFO);
                    si->MaxNameLen = 32;
                    if (SymFromAddr(GetCurrentProcess(), fn, &disp, si))
                        fn = si->Address;
                }
#endif
                if (d == 0 && (c = sample_slot(fn, 1))) (*c)++;
                for (k = 0; k < nseen && seen[k] != fn; k++) ;
                if (k == nseen) {
                    seen[nseen++] = fn;
                    if ((c = sample_slot(fn, 0))) (*c)++;
                }
            }
            total++;
        }
#if !defined(_WIN32)
        else            /* POSIX resumed it as soon as its context was read */
#endif
        ResumeThread(s_sample_target);
        if (GetTickCount() - last >= 30000) {
            static int window = -1;
            last = GetTickCount();
            sample_report(total);
            /* RECOMP_SAMPLE_WINDOW=1: each report covers only the last 30 s,
             * so a steady scene (the hub) is not diluted by boot and menus. */
            if (window < 0)
                window = getenv("RECOMP_SAMPLE_WINDOW") != NULL;
            if (window) {
                memset(s_samples, 0, sizeof s_samples);
                total = 0;
            }
        }
    }
}

/* RECOMP_SAMPLE_GPU=2: raster worker 0 instead. Its pool starts with the
 * first draw, after boot, so wait for it here rather than in sampler_start. */
extern void *nv2a_pb_exec_raster_thread(int k);

static DWORD WINAPI raster_sampler_main(LPVOID param)
{
    while (!nv2a_pb_exec_raster_thread(0))
        Sleep(500);
    DuplicateHandle(GetCurrentProcess(), (HANDLE)nv2a_pb_exec_raster_thread(0),
                    GetCurrentProcess(), &s_sample_target, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    fprintf(stderr, "[SAMPLE] sampling raster worker 0\n");
    return sampler_main(param);
}

static void sampler_start(void)
{
    const char *e = getenv("RECOMP_SAMPLE_MS");
    const char *g = getenv("RECOMP_SAMPLE_GPU");
    if (!e || atoi(e) <= 0) return;
    if (g && atoi(g) == 2) {
        CreateThread(NULL, 0, raster_sampler_main, (LPVOID)(uintptr_t)atoi(e), 0, NULL);
        return;
    }
    /* RECOMP_SAMPLE_GPU=1 samples the NV2A worker thread instead -- where the
     * pushbuffer executor runs under RECOMP_PB_EXEC. When the main thread is
     * stuck waiting for pushbuffer space, that is the thread worth timing. */
    if (getenv("RECOMP_SAMPLE_GPU") && xbox_Nv2aWorkerThread()) {
        DuplicateHandle(GetCurrentProcess(), (HANDLE)xbox_Nv2aWorkerThread(),
                        GetCurrentProcess(), &s_sample_target, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        CreateThread(NULL, 0, sampler_main, (LPVOID)(uintptr_t)atoi(e), 0, NULL);
        fprintf(stderr, "[SAMPLE] sampling the NV2A worker thread every %d ms\n", atoi(e));
        return;
    }
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &s_sample_target, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CreateThread(NULL, 0, sampler_main, (LPVOID)(uintptr_t)atoi(e), 0, NULL);
    fprintf(stderr, "[SAMPLE] sampling the guest main thread every %d ms\n", atoi(e));
}

/* RECOMP_MEMDUMP_SECS=<n>: every n seconds, write guest RAM (the 64 MB from
 * VA 0) to memdump.bin in the current directory, overwriting the previous
 * snapshot -- boot speed varies run to run, so the last one before a kill or
 * the watchdog is the one that shows the stuck state. A hang that lives in game
 * data rather than code -- a name table full of garbage, a struct never
 * filled -- is answered by searching the snapshot offline, which no trace
 * that has to be decided up front can do. Racy by nature (the guest keeps
 * running while it is copied); it is a snapshot, not a checkpoint. */
static DWORD WINAPI memdump_main(LPVOID param)
{
    unsigned n = 0;
    for (;;) {
        FILE *f;
        Sleep((DWORD)(uintptr_t)param * 1000);
        f = fopen("memdump.bin", "wb");
        if (!f) {
            fprintf(stderr, "[MEMDUMP] cannot open memdump.bin\n");
            return 0;
        }
        /* The whole mapped range, not just 64 MB of RAM: with
         * xbox_SetMapSize(128 MB) the heap -- and the stacks of threads
         * spawned late -- live above 0x04000000. */
        fwrite((const void *)(uintptr_t)g_xbox_mem_offset, 1,
               g_xbox_map_size ? g_xbox_map_size : 0x04000000, f);
        fclose(f);
        fprintf(stderr, "[MEMDUMP] snapshot #%u of guest RAM in memdump.bin\n",
                ++n);
    }
}

/* DirectSound's GP-DSP command doorbell, acked from the guest's own pointers.
 *
 * Audio init (sub_0021C8B0) calls DirectSoundCreate(.., &g_pDS = 0xEC4C0C, ..)
 * and then downloads media\se\dsp.bin. In the download, sub_003341BE (under the
 * DSOUND lock 0x00339ADC) writes command 3 to scratch+0x810 and spins until
 * the GP DSP clears it. The runtime's GP DSP is a passthrough stub, so the
 * word never clears and the main thread hangs holding the lock.
 *
 * This used to default RECOMP_DSP_ACK to a fixed 0x803D0810, but the scratch
 * is a contiguous allocation and moves whenever anything allocates before it.
 * With D3D lifted in full it moved to 0x80A20810 and the hang came back. So
 * follow the same chain DirectSound does:
 *   doorbell = [[[[[[0xEC4C0C]+0x4]+0x14]+0x8]+0x10]] + 0x810
 * and clear the word whenever it is non-zero. The chain was found from a hang
 * dump: the spinning frame's `this` (0x80A44134) is stored at [g_pDS+4]+0x14.
 * The +0xC path that reading the call chain suggested reaches a different
 * object whose [this+8] is 0. This is a handshake ack, not a
 * DSP: anything DirectSound reads back from the GP stays wrong. An explicit
 * RECOMP_DSP_ACK hands the job back to the runtime and turns this off. */
static int guest_ptr_ok(uint32_t va)
{
    return (va >= 0x00010000u && va < 0x04000000u)
        || (va >= 0x80000000u && va < 0x84000000u);
}

static void dsp_doorbell_pass(void)
{
    static const uint32_t chain[] = { 0x04, 0x14, 0x08, 0x10, 0x00 };
    static uint32_t last, last_root;
    static unsigned acks;
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t p = *(volatile const uint32_t *)(mem + 0x00EC4C0Cu);
    size_t i;

    if (p != last_root) {               /* log each new chain once */
        uint32_t q = p;
        fprintf(stderr, "[DSP] chain root 0x%08X", q);
        for (i = 0; i < sizeof(chain) / sizeof(chain[0]) && guest_ptr_ok(q); i++) {
            q = *(volatile const uint32_t *)(mem + q + chain[i]);
            fprintf(stderr, " -> 0x%08X", q);
        }
        fprintf(stderr, "\n");
        last_root = p;
    }
    for (i = 0; i < sizeof(chain) / sizeof(chain[0]) && guest_ptr_ok(p); i++)
        p = *(volatile const uint32_t *)(mem + p + chain[i]);
    if (i < sizeof(chain) / sizeof(chain[0]) || !guest_ptr_ok(p)) {
        static size_t shown_i = (size_t)-1;
        static uint32_t shown_p;
        if (p && (i != shown_i || p != shown_p)) {
            fprintf(stderr, "[DSP] doorbell chain stops at step %u: 0x%08X\n",
                    (unsigned)i, p);
            shown_i = i;
            shown_p = p;
        }
        return;                         /* DirectSound not up yet */
    }
    p += 0x810;
    if (p != last) {
        fprintf(stderr, "[DSP] DirectSound doorbell at 0x%08X\n", p);
        last = p;
    }
    if (*(volatile uint32_t *)(uintptr_t)(mem + p)) {
        *(volatile uint32_t *)(uintptr_t)(mem + p) = 0;
        if (++acks <= 3)
            fprintf(stderr, "[DSP] doorbell command acknowledged\n");
    }
}

static DWORD WINAPI dsp_doorbell_main(LPVOID unused)
{
    (void)unused;
    fprintf(stderr, "[DSP] doorbell watcher running (mem=%p)\n",
            (void *)(uintptr_t)g_xbox_mem_offset);
    for (;;) {
        Sleep(10);
        dsp_doorbell_pass();
    }
}

/* DirectSound's timer list, when its DPC spins (results-screen freeze).
 *
 * The timer DPC 0x00334A70 -> sub_003348A8 walks a list at ctx+0x4A8, sorted
 * by absolute due time: it reads the clock, and if the head entry is due it
 * unlinks it and calls [entry+0xC]->vtbl[0x24](entry+0x10, entry+0x14);
 * otherwise it re-arms the KTIMER at ctx+0x4E8 and returns. One clock read per
 * pass, so the runtime's [DPCSPIN] count is this loop's iteration count.
 * Entries are 32-byte slots at owner+0x88 (owner vtable 0x36F584, timer ids
 * 0..5): +0 Flink, +4 Blink, +8 flags (2 = queued, 4 = periodic), +0xC owner,
 * +0x10 id, +0x14 arg, +0x18 due. It can only loop forever if an entry
 * stays due at the head -- a corrupt link or a callback that re-queues it --
 * so this prints the links, checks each Blink, and stops at a repeat. */
#define DS_TIMER_DPC 0x00334A70u

/* DirectSound's APU DPC (0x00334BDF -> sub_00334B43 -> sub_003346BE), the one
 * that froze stage 01-01 (2026-10-12 capture). Its second loop pops voices
 * off the "idle, being retired" list at ctx+0x4A0 (voice object + 0x54) and
 * calls their vtbl+0x18 (sub_003371D1) until the list is empty. The APU ISR
 * fills that list: SE2FE_IDLE_VOICE -> sub_003347D1 -> sub_00335C18(v, 1)
 * unlinks the voice from the hardware list and parks it there, unless
 * DirectSound's busy count ctx+0x84 is set. Printed: each parked voice's
 * handle, hardware list, flags and notifier-3 status, and that count. */
#define DS_APU_DPC 0x00334BDFu

static void dsound_apu_dpc_dump(const uint8_t *mem, uint32_t ctx,
                                unsigned clock_reads)
{
#define G32(a) (*(const uint32_t *)(mem + (a)))
#define G16(a) (*(const uint16_t *)(mem + (a)))
#define G8(a)  (*(const uint8_t *)(mem + (a)))
    uint32_t head = ctx + 0x4A0, e, prev;
    unsigned n = 0;

    fprintf(stderr, "  [DSAPU] ctx 0x%08X busy %u ISR bits %08X DPC bits %08X;"
            " %u clock reads; parked list Flink %08X Blink %08X\n", ctx,
            G32(ctx + 0x84), G32(ctx + 0x4B8), G32(ctx + 0x4C0), clock_reads,
            G32(head), G32(head + 4));
    for (prev = head, e = G32(head); e != head && n < 16; prev = e, e = G32(e), n++) {
        uint32_t v = e - 0x54, ntf;
        if (!guest_ptr_ok(e)) {
            fprintf(stderr, "  [DSAPU]   bad link 0x%08X after 0x%08X\n", e, prev);
            break;
        }
        ntf = G32(v + 0x68);
        fprintf(stderr, "  [DSAPU]   voice obj 0x%08X%s handle %u (n %u) hw list %u"
                " flags %04X ntf3 %02X\n", v,
                G32(e + 4) == prev ? "" : " (Blink != prev!)", G16(v + 0xC),
                G8(v + 0x64), G8(v + 0x65), G16(v + 0x12),
                guest_ptr_ok(ntf) ? G8(ntf + 0x3F) : 0xEE);
    }
#undef G8
#undef G16
#undef G32
    fflush(stderr);
}

static void dsound_timer_spin_hook(uint32_t dpc_va, uint32_t routine,
                                   uint32_t ctx, unsigned clock_reads)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t head, e, prev, seen[32];
    LARGE_INTEGER now;
    unsigned n = 0, i;

    (void)dpc_va;
    if (routine == DS_APU_DPC && guest_ptr_ok(ctx)) {
        dsound_apu_dpc_dump(mem, ctx, clock_reads);
        return;
    }
    if (routine != DS_TIMER_DPC || !guest_ptr_ok(ctx))
        return;
    xbox_KeQuerySystemTime(&now);
#define G32(a) (*(const uint32_t *)(mem + (a)))
    head = ctx + 0x4A8;
    fprintf(stderr, "  [DSTIMER] ctx 0x%08X now %08X:%08X head 0x%08X Flink %08X"
            " Blink %08X; immediate list Flink %08X\n", ctx,
            (uint32_t)(now.QuadPart >> 32), (uint32_t)now.QuadPart, head,
            G32(head), G32(head + 4), G32(ctx + 0x4B0));
    for (prev = head, e = G32(head); e != head && n < 32; prev = e, e = G32(e)) {
        long long due;
        if (!guest_ptr_ok(e)) {
            fprintf(stderr, "  [DSTIMER]   bad link 0x%08X after 0x%08X\n", e, prev);
            break;
        }
        for (i = 0; i < n; i++)
            if (seen[i] == e)
                break;
        due = (long long)(((uint64_t)G32(e + 0x1C) << 32) | G32(e + 0x18));
        fprintf(stderr, "  [DSTIMER]   0x%08X F %08X B %08X%s flags %X owner %08X"
                " (vtbl %08X) id %u arg %08X due %+lld us%s\n", e, G32(e),
                G32(e + 4), G32(e + 4) == prev ? "" : " (Blink != prev!)",
                G32(e + 8), G32(e + 0xC),
                guest_ptr_ok(G32(e + 0xC)) ? G32(G32(e + 0xC)) : 0,
                G32(e + 0x10), G32(e + 0x14), (due - now.QuadPart) / 10,
                i < n ? " REPEAT: list is cyclic" : "");
        if (i < n)
            break;
        seen[n++] = e;
    }
#undef G32
    fflush(stderr);
}

static void dsp_doorbell_start(void)
{
    if (getenv("RECOMP_DSP_ACK"))
        return;
    /* Under a guest-CPU schedule the runtime runs it: at every timer turn,
     * and at the yield point of DirectSound's own spin (sub_003341BE). */
    if (xbox_SchedMode()) {
        xbox_SchedAddTick(dsp_doorbell_pass);
        xbox_SchedAddPump(dsp_doorbell_pass);
        return;
    }
    CloseHandle(CreateThread(NULL, 0, dsp_doorbell_main, NULL, 0, NULL));
}

/* "GPU always idle": a WORKAROUND, like the DSP doorbell ack above.
 *
 * D3D tracks GPU progress with fence times kept in its device (pointer at
 * 0x2F1FB8). [dev+0x2C] is the CPU's next fence time, advanced by 2 per
 * fence. [[dev+0x30]] is the last time the GPU completed, which on hardware
 * the GPU writes back from a semaphore release in the pushbuffer. D3D calls
 * the GPU idle when gpu + 2 == cpu (sub_002E7E80). sub_002E8110 waits,
 * retrying KeWaitForSingleObject, until the GPU passes the fence it wants.
 * Nothing here executes the pushbuffer by default, so the GPU time never moved
 * and the first frame waited forever (ordinal 159 x364M).
 *
 * So report every fence as completed: gpu = cpu - 2. sub_002E7E80 also spins
 * until NV2A register 0x400B10 (bits 2-6) agrees with gpu << 2; the runtime's
 * aperture is plain RAM, so echo that too. Nothing is drawn by this. It only
 * lets the title stop waiting for a GPU that does not exist yet.
 * RECOMP_GPU_IDLE_SHIM=0 turns it off. Under RECOMP_PB_EXEC (the default)
 * it is not started: the executor releases the fences (gpu_idle_start). */
static void gpu_idle_pass(void)
{
    static unsigned announced;
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t dev, gp, cpu, gpu;

    dev = *(volatile const uint32_t *)(mem + 0x002F1FB8u);
    if (!guest_ptr_ok(dev))
        return;
    gp = *(volatile const uint32_t *)(mem + dev + 0x30);
    if (!guest_ptr_ok(gp))
        return;
    cpu = *(volatile const uint32_t *)(mem + dev + 0x2C);
    gpu = cpu - 2;
    if (*(volatile uint32_t *)(uintptr_t)(mem + gp) != gpu)
        *(volatile uint32_t *)(uintptr_t)(mem + gp) = gpu;
    *(volatile uint32_t *)(uintptr_t)(mem + 0xFD400B10u) = (gpu << 2) & 0x7C;
    if (!announced++)
        fprintf(stderr, "[GPU] idle shim: device 0x%08X, fence at 0x%08X "
                        "(workaround: every fence reported complete)\n", dev, gp);
}

static DWORD WINAPI gpu_idle_main(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(1);
        gpu_idle_pass();
    }
}

static void gpu_idle_start(void)
{
    const char *e = getenv("RECOMP_GPU_IDLE_SHIM");
    if (e && *e == '0')
        return;
    /* With the pushbuffer executor the fences are real: the stream's
     * semaphore releases ([dev+0x30] is where D3D points the semaphore DMA
     * object) are written as the executor reaches them. The shim's 1 ms poll
     * held the title in sub_002E8110 for a fifth of its time, and reporting
     * fences complete before the executor got there let the title reuse
     * buffers the GPU had not read yet. RECOMP_GPU_IDLE_SHIM=1 forces it. */
    if (getenv("RECOMP_PB_EXEC") && !(e && *e == '1')) {
        xbox_Nv2aSemaphoreAt(0x002F1FB8u, 0x30);
        return;
    }
    /* Under a guest-CPU schedule: at every timer turn, and at the yield
     * point of D3D's wait for it (sub_002E7E80). */
    if (xbox_SchedMode()) {
        xbox_SchedAddTick(gpu_idle_pass);
        xbox_SchedAddPump(gpu_idle_pass);
        return;
    }
    CloseHandle(CreateThread(NULL, 0, gpu_idle_main, NULL, 0, NULL));
}

/* Keeps a fully buffered stderr live; see WinMain. */
static DWORD WINAPI stderr_flusher(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(500);
        fflush(stderr);
    }
}

/* RECOMP_OCTREE_WATCH=1: the game's triangle octree (built by sub_001642A0),
 * watched from outside. Its node pool is 2000 nodes at 0xCAE8A8 (counter
 * 0xCC9E30, no bounds check -- node 2000 is the counter itself), its
 * triangle-link pool 60000 at 0xBACB98 (counter 0xCCA048), and a node splits
 * while it holds more than 50 triangles. A triangle that overlaps every box
 * -- NaN or enormous vertices -- never thins out, the split never stops, and
 * the pools overflow into the counters (P3's one-off crash in sub_001645D0).
 * The triangle store before it is unbounded as well: 8000 records of 0x48 at
 * 0xC21EA8 (count 0xCC9E2C, sub_00164ED0), ending exactly where the node
 * pool begins, so a scene with more than 8000 collision triangles overwrites
 * the nodes. Polls every millisecond: per build, the peak of both counters,
 * the triangle count, and how many vertex coordinates are non-finite or
 * beyond the root's +-10000 bounds; loud when a build nears the node limit. */
static DWORD WINAPI octree_watch_main(LPVOID unused)
{
    const uint8_t *mem = (const uint8_t *)(uintptr_t)g_xbox_mem_offset;
    uint32_t peak_n = 0, peak_l = 0, last_n = 0, builds = 0, still = 0;
    int warned = 0;
    (void)unused;
    for (;;) {
        uint32_t n = *(volatile const uint32_t *)(mem + 0xCC9E30);
        uint32_t l = *(volatile const uint32_t *)(mem + 0xCCA048);
        still = (n == last_n) ? still + 1 : 0;
        /* A build is over when its counter stops for half a second, or when
         * the next one resets it. */
        if (peak_n && (n < last_n || still == 500)) {
            uint32_t tris = *(volatile const uint32_t *)(mem + 0xCC9E2C), i, j, bad = 0;
            if (tris > 8000) tris = 8000;
            for (i = 0; i < tris; i++)
                for (j = 0; j < 9; j++) {
                    float v = *(const float *)(mem + 0xC21EA8 + i * 0x48 + j * 4);
                    if (!(v > -10000.0f && v < 10000.0f))
                        bad++;
                }
            if (builds++ < 50 || peak_n > 1500)
                fprintf(stderr, "  [OCTREE] build %u: nodes peak %u/2000, links "
                        "peak %u/60000, %u/8000 triangles (%u bad coords)\n",
                        builds, peak_n, peak_l, tris, bad);
            peak_n = peak_l = 0;
            warned = 0;
        }
        if (n > peak_n) peak_n = n;
        if (l > peak_l) peak_l = l;
        if (n >= 1900 && !warned) {
            warned = 1;
            fprintf(stderr, "  [OCTREE] node pool at %u/2000 -- about to "
                    "overflow (links %u, triangles %u)\n", n, l,
                    *(volatile const uint32_t *)(mem + 0xCC9E2C));
        }
        last_n = n;
        Sleep(1);
    }
}

static void memdump_start(void)
{
    if (getenv("RECOMP_OCTREE_WATCH"))
        CloseHandle(CreateThread(NULL, 0, octree_watch_main, NULL, 0, NULL));
    const char *e = getenv("RECOMP_MEMDUMP_SECS");
    if (!e || atoi(e) <= 0) return;
    CloseHandle(CreateThread(NULL, 0, memdump_main,
                             (LPVOID)(uintptr_t)atoi(e), 0, NULL));
}

static void print_guest_context(void *rip)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)rip, &disp, sym))
        fprintf(stderr, "  in %s+0x%llX\n",
                sym->Name, (unsigned long long)disp);

    if (g_xbox_mem_offset && g_esp) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 256 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
        /* When the frame is gone, say so, and fall back on what is left.
         *
         * A fault inside a block copy leaves no return address in range: the
         * copy is a host memmove and its caller's frame is further up than
         * this reaches. Worse, a copy that ran past its buffer overwrites the
         * stack itself, so the list comes out empty exactly when it is most
         * needed -- and an empty list under a promising heading reads like
         * "nothing to see" rather than "the evidence is destroyed". */
        if (!shown)
            fprintf(stderr, "    (no return addresses in range)\n");

        /* Everything below only pays for itself when the scan came back
         * empty. A crash where the chain was recovered already says what
         * happened, and burying that under another twenty lines makes the
         * common report worse to read. */
        if (!shown) {
            uint32_t k;

            /* The indirect-call history, which the stack cannot overwrite.
             * recomp_manual.c prints this too, but only from
             * recomp_icall_fail_log -- a page fault inside a block copy
             * never reaches that path, and this is the same information by
             * the route the fault actually took.
             *
             * Declared here rather than included: main.c does not pull in
             * recomp_types.h, and recomp_manual.c spells it the same way. */
            extern volatile uint32_t g_icall_trace[16];
            extern volatile uint32_t g_icall_trace_idx;

            fprintf(stderr, "  recent ICALL targets:");
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X",
                        g_icall_trace[(g_icall_trace_idx + k) & 15]);
            fprintf(stderr, "\n");

            /* The frame unfiltered. The pointers and lengths handed to
             * whatever faulted live here and look nothing like code, so the
             * filter above drops exactly what is wanted. On the crash this
             * was written for, one of these words was the end of the mapped
             * RAM mirror -- which is what identified the fault as a copy
             * running off its buffer rather than a stray pointer. */
            for (i = 0; i < 20; i++)
                fprintf(stderr, "    raw[esp+%-4d] 0x%08X\n", i * 4, sp[i]);
        }
    }
}

/* The emulated APU.
 *
 * Two entry points that had no callers anywhere in the tree. apu_mmio_hook.c
 * documents apu_hook_handle_mmio as "called from VEH in main.c" and no main.c
 * called it; apu_decode_and_handle's first line is `if (!g_apu_state) return
 * false` and nothing assigned g_apu_state. So with RECOMP_AC97_READY set the
 * APU's registers were unmapped to be trapped, and every trap was then
 * declined and surfaced as an access violation on the first register
 * DirectSound touched.
 *
 * Declared rather than included so this does not depend on src/apu being on
 * the include path. */
typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr,
                                 uint32_t fault_xbox_va, int is_write);

/* The trapped span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not the whole MCPX aperture. AC'97 above it stays plain memory,
 * which is what the codec-ready bit needs. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u

/* Every way the process can end should say so. A run died ~8 min in with no
 * [CRASH], no watchdog and no exit message: the VEH below only reported
 * access violations. These cover the rest.
 *
 * A host stack overflow runs this on the few KB left past the guard page,
 * so that path formats into a local buffer and writes straight to the stderr
 * handle -- no stdio, which could need its lock or more stack. */
static void raw_stderr(const char *s)
{
#if defined(_WIN32)
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)strlen(s), &w, NULL);
#else
    ssize_t r = write(2, s, strlen(s));
    (void)r;
#endif
}

static void exit_report_atexit(void)
{
    fprintf(stderr, "[EXIT] process exiting through the CRT (exit/return)\n");
    fflush(stderr);
}

static void exit_report_sigabrt(int sig)
{
    (void)sig;
    fflush(stderr);
    raw_stderr("[CRASH] abort() (SIGABRT)\n");
}

static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    if (code == EXCEPTION_STACK_OVERFLOW) {
        char b[192];
        _snprintf(b, sizeof(b), "\n[CRASH] host STACK OVERFLOW, thread %lu, "
                  "RIP=%p, guest esp=0x%08X\n", GetCurrentThreadId(),
                  (void *)ep->ContextRecord->Rip, g_esp);
        b[sizeof(b) - 1] = 0;
        raw_stderr(b);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_PRIV_INSTRUCTION
            || code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_INT_OVERFLOW
            || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED
            || code == EXCEPTION_NONCONTINUABLE_EXCEPTION) {
        fprintf(stderr, "[CRASH] exception 0x%08lX at RIP=0x%llX, host thread %lu\n",
                code, (unsigned long long)ep->ContextRecord->Rip,
                GetCurrentThreadId());
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
                g_eax, g_ecx, g_edx, g_esp);
        print_guest_context((void *)ep->ContextRecord->Rip);
        fflush(stderr);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault_addr = ep->ExceptionRecord->ExceptionInformation[1];

        /*
         * GPU register probe at 0xFD000000 range.
         * Some games probe NV2A registers directly. On real hardware this
         * returns GPU state; here we just skip the instruction.
         * TODO: Implement mini x86-64 decoder for instruction skipping,
         * or connect to the xbox_nv2a library for proper handling.
         */
        if (fault_addr >= 0xFD000000 && fault_addr < 0xFE000000) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /* APU registers -> the emulated APU.
         *
         * The other half of RECOMP_AC97_READY: reporting the codec ready is
         * what lets DirectSoundCreate past its first gate, and from there it
         * drives the APU directly, so the registers have to fault to be seen.
         * "Handled" means the access was decoded and the instruction stepped
         * over, so execution resumes instead of unwinding. */
        {
            uint32_t xbox_va =
                (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset);

            if (xbox_va >= APU_TRAP_BASE && xbox_va < APU_TRAP_END
                    && apu_hook_handle_mmio(
                           ep->ContextRecord, fault_addr, xbox_va,
                           ep->ExceptionRecord->ExceptionInformation[0]
                               ? 1 : 0)) {
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            /* USB host controllers -> the OHCI model (RECOMP_USB=1). XAPI
             * drives the gamepad through these registers directly, and
             * xbox_OhciInit traps the two 4 KB apertures so they land here. */
            if (xbox_OhciOwnsAddress(xbox_va)
                    && xbox_OhciHandleMmio(ep->ContextRecord, xbox_va))
                return EXCEPTION_CONTINUE_EXECUTION;
        }

        fprintf(stderr, "[CRASH] Access violation at RIP=0x%llX, fault addr=0x%llX (%s)\n",
            (unsigned long long)ep->ContextRecord->Rip,
            (unsigned long long)fault_addr,
            ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read");
        fprintf(stderr, "  Xbox regs: eax=0x%08X ecx=0x%08X edx=0x%08X esp=0x%08X\n",
            g_eax, g_ecx, g_edx, g_esp);
        fprintf(stderr, "  Xbox regs: ebx=0x%08X esi=0x%08X edi=0x%08X\n",
            g_ebx, g_esi, g_edi);
        fprintf(stderr, "  Xbox VA of fault: 0x%08X\n",
            (uint32_t)(fault_addr - (uintptr_t)g_xbox_mem_offset));
        print_guest_context((void *)ep->ContextRecord->Rip);

        /*
         * TODO: Add game-specific diagnostics here. Examples:
         *
         * Dump CRT heap handle:
         *   uint32_t heap = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + HEAP_HANDLE_VA);
         *   fprintf(stderr, "  CRT heap handle: 0x%08X\n", heap);
         *
         * Dump game state:
         *   uint32_t state = *(uint32_t *)((uint8_t *)g_xbox_mem_offset + GAME_STATE_VA);
         *   fprintf(stderr, "  Game state: %u\n", state);
         */

        /* Native call chain, unwound from the fault context and named through
         * the PDB. (This used to filter raw stack words to 0x14xxxxxxx, the
         * non-ASLR image base, so under ASLR it never printed a frame.) The
         * thread id and fs base say *which* thread faulted: guest code also
         * runs on DPC, interrupt and APU threads, not just the game's own. */
        {
            CONTEXT ctx = *ep->ContextRecord;
            char sbuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)sbuf;
            int d;

            fprintf(stderr, "  host thread %lu, g_fs_base=0x%08X\n",
                    GetCurrentThreadId(), g_fs_base);
            fprintf(stderr, "  Native stack:\n");
#if !defined(_WIN32)
            /* POSIX: the handler runs on the faulting thread, so its own
             * backtrace runs through the signal frame into the fault. */
            {
                PVOID fr[32];
                int nf = CaptureStackBackTrace(0, 32, fr, NULL);
                (void)ctx;
                for (d = 0; d < nf; d++) {
                    DWORD64 disp = 0;
                    memset(sbuf, 0, sizeof(sbuf));
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 255;
                    fprintf(stderr, "    [%2d] %p %s+0x%llX\n", d, fr[d],
                            SymFromAddr(GetCurrentProcess(), (DWORD64)(uintptr_t)fr[d],
                                        &disp, sym) ? sym->Name : "?",
                            (unsigned long long)disp);
                }
            }
#else
            for (d = 0; d < 24 && ctx.Rip; d++) {
                DWORD64 base = 0, disp = 0;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                memset(sbuf, 0, sizeof(sbuf));
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                fprintf(stderr, "    [%2d] 0x%llX %s+0x%llX\n", d,
                        (unsigned long long)ctx.Rip,
                        SymFromAddr(GetCurrentProcess(), ctx.Rip, &disp, sym)
                            ? sym->Name : "?",
                        (unsigned long long)disp);
                if (!rf) {
                    ctx.Rip = *(DWORD64 *)ctx.Rsp;
                    ctx.Rsp += 8;
                } else {
                    void *hd; DWORD64 ef;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf,
                                     &ctx, &hd, &ef, NULL);
                }
            }
#endif
        }
        fflush(stderr);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

/* s_headless (declared above settings_game_dir) is set from RECOMP_HEADLESS
 * in WinMain: no window, no dialogs. */

extern void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars);

/* A crash nothing handled. The VEH above sees first-chance exceptions and
 * only logs; this runs once the process is going down, so it is the place to
 * tell a player who has no console that the game died, and where the
 * details are. */
static LONG WINAPI fatal_filter(PEXCEPTION_POINTERS ep)
{
    fflush(stderr);
    if (!s_headless) {
        char msg[256];
        _snprintf(msg, sizeof(msg),
                  "Blinx 2 (recomp) crashed: exception 0x%08lX at 0x%llX.\n\n"
                  "Details are in blinx2_recomp.log and xbox_kernel.log next to "
                  "the exe.",
                  ep->ExceptionRecord->ExceptionCode,
                  (unsigned long long)ep->ContextRecord->Rip);
        msg[sizeof(msg) - 1] = 0;
        MessageBoxA(NULL, msg, "Blinx 2 (recomp)", MB_ICONERROR | MB_OK);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

/* ── WinMain ───────────────────────────────────────────────── */

/* <base>.log -> <base>.prev-<its last write time>.log, keeping the newest
 * `keep` of those. In the working directory, before anything opens the log. */
static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

#if !defined(_WIN32)
/* POSIX: <name> -> <stem>.prev-<mtime of `stamp`, local time><ext>, then keep
 * the newest `keep` matching <stem>.prev-*<ext>. */
static void rotate_posix(const char *name, const char *stamp, const char *stem,
                         const char *ext, int keep)
{
    char prev[MAX_PATH], pat[MAX_PATH];
    struct stat st;
    glob_t g;
    size_t i;

    if (stat(name, &st) == 0) {
        struct tm tm;
        struct stat ts;
        time_t t = (stamp && stat(stamp, &ts) == 0) ? ts.st_mtime : st.st_mtime;
        localtime_r(&t, &tm);
        snprintf(prev, sizeof prev, "%s.prev-%04d%02d%02d-%02d%02d%02d%s", stem,
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, ext);
        rename(name, prev);
    }
    snprintf(pat, sizeof pat, "%s.prev-*%s", stem, ext);
    if (glob(pat, 0, NULL, &g) != 0)
        return;
    /* glob sorts, and the names are timestamped: oldest first. */
    for (i = 0; i + (size_t)keep < g.gl_pathc; i++)
        unlink(g.gl_pathv[i]);
    globfree(&g);
}

static void rotate_log(const char *base, int keep)
{
    char cur[MAX_PATH];
    snprintf(cur, sizeof cur, "%s.log", base);
    rotate_posix(cur, NULL, base, ".log", keep);
}

static void rotate_rec(int keep)
{
    rotate_posix("blinx2_input.rec", "blinx2_recomp.log", "blinx2_input", ".rec", keep);
}
#else
static void rotate_log(const char *base, int keep)
{
    char cur[MAX_PATH], prev[MAX_PATH], pat[MAX_PATH];
    static char names[64][MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;

    snprintf(cur, sizeof cur, "%s.log", base);
    if (GetFileAttributesExA(cur, GetFileExInfoStandard, &fa)) {
        FILETIME lt;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        snprintf(prev, sizeof prev, "%s.prev-%04u%02u%02u-%02u%02u%02u.log", base,
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        MoveFileExA(cur, prev, MOVEFILE_REPLACE_EXISTING);
    }
    snprintf(pat, sizeof pat, "%s.prev-*.log", base);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (n < 64)
            snprintf(names[n++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, MAX_PATH, cmp_str);   /* oldest first: timestamped */
    for (i = 0; i + keep < n; i++)
        DeleteFileA(names[i]);
}

/* blinx2_input.rec (the input recording, see recomp_manual.c) goes with the
 * log of the same run: it is renamed with blinx2_recomp.log's timestamp, so
 * blinx2_input.prev-<T>.rec pairs with blinx2_recomp.prev-<T>.log. Call
 * before rotate_log moves that log. Keeps the newest `keep`. */
static void rotate_rec(int keep)
{
    static char names[64][MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i;

    if (GetFileAttributesExA("blinx2_input.rec", GetFileExInfoStandard, &fa)) {
        char prev[MAX_PATH];
        FILETIME lt;
        SYSTEMTIME st;
        if (!GetFileAttributesExA("blinx2_recomp.log", GetFileExInfoStandard, &fa))
            GetFileAttributesExA("blinx2_input.rec", GetFileExInfoStandard, &fa);
        FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt);
        FileTimeToSystemTime(&lt, &st);
        snprintf(prev, sizeof prev,
                 "blinx2_input.prev-%04u%02u%02u-%02u%02u%02u.rec",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        MoveFileExA("blinx2_input.rec", prev, MOVEFILE_REPLACE_EXISTING);
    }
    h = FindFirstFileA("blinx2_input.prev-*.rec", &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (n < 64)
            snprintf(names[n++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(names, (size_t)n, MAX_PATH, cmp_str);
    for (i = 0; i + keep < n; i++)
        DeleteFileA(names[i]);
}
#endif

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    void *xbe_data = NULL;
    size_t xbe_size = 0;

    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    /* Run from the exe's own folder. xbox_kernel.log, the stderr log below
     * and the runtime's relative paths all land in the working directory,
     * and a shell launch does not always make that the exe's folder (one
     * started through explorer.exe ran in System32 and wrote nothing). */
    {
#if defined(_WIN32)
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            char *slash = strrchr(path, '\\');
            if (slash) {
                *slash = 0;
                SetCurrentDirectoryA(path);
            }
        }
#else
        char path[4096];
        ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
        if (n > 0) {
            char *slash;
            path[n] = 0;
            slash = strrchr(path, '/');
            if (slash && slash != path) {
                *slash = 0;
                snprintf(g_exe_dir, sizeof g_exe_dir, "%s", path);
                if (chdir(path) != 0)
                    perror("chdir");
            }
        }
        /* A read-only install (AppImage, a system or Flatpak install): the
         * logs, the input recording and blinx2_settings.ini go to
         * $XDG_STATE_HOME/blinx2-recomp (~/.local/state/blinx2-recomp)
         * instead. Saves are elsewhere already (XDG data dir, game_files). */
        if (access(".", W_OK) != 0) {
            const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
            char dir[4096], *p;
            if (st && *st)
                snprintf(dir, sizeof dir, "%s/blinx2-recomp", st);
            else
                snprintf(dir, sizeof dir, "%s/.local/state/blinx2-recomp",
                         home && *home ? home : "/tmp");
            for (p = dir + 1; *p; p++)
                if (*p == '/') {
                    *p = 0;
                    mkdir(dir, 0755);
                    *p = '/';
                }
            mkdir(dir, 0755);
            if (chdir(dir) != 0)
                perror(dir);
        }
#endif
    }

#if defined(_WIN32)
    /* Restarted into a network game by the menu (RECOMP_NET_PARENT, the old
     * process's id): let it finish dying first. It still held
     * blinx2_recomp.log, so the rotation and the reopen below failed and the
     * network game ran with no log. */
    {
        const char *pp = getenv("RECOMP_NET_PARENT");
        if (pp && *pp) {
            HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)strtoul(pp, NULL, 10));
            if (ph) {
                WaitForSingleObject(ph, 5000);
                CloseHandle(ph);
            }
        }
    }
#endif

    /* Keep the last runs' logs: a crash log used to be overwritten by the
     * next launch, before anyone had read it. */
    rotate_rec(5);                    /* first: it reads the log's timestamp */
    rotate_log("blinx2_recomp", 5);
    rotate_log("xbox_kernel", 5);

    /* Launched by double-click there is no console and no redirection, so
     * stderr goes nowhere and a crash leaves no trace. Write it to
     * blinx2_recomp.log in the working directory instead, and point the
     * raw-handle writers (raw_stderr) at the same file. stdout (a few banner
     * lines) stays put: reopening it onto the same file failed fast in the
     * CRT (0xC0000409). */
#if !defined(_WIN32)
    /* POSIX: a desktop launch hands down /dev/null (or nothing); a terminal
     * or a redirection is left alone. */
    {
        struct stat st;
        if (fstat(2, &st) != 0 || (S_ISCHR(st.st_mode) && !isatty(2)))
            if (!freopen("blinx2_recomp.log", "w", stderr))
                perror("blinx2_recomp.log");
    }
#else
    {
        HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
        DWORD type = (h && h != INVALID_HANDLE_VALUE) ? GetFileType(h)
                                                      : FILE_TYPE_UNKNOWN, mode;
        /* No handle, or a character device that is not a console (NUL, which
         * is what a shell launch can hand down). Files and pipes -- a
         * redirected run -- are left alone. */
        if (_fileno(stderr) < 0 || type == FILE_TYPE_UNKNOWN
                || (type == FILE_TYPE_CHAR && !GetConsoleMode(h, &mode))) {
            FILE *f;
            if (freopen_s(&f, "blinx2_recomp.log", "w", stderr) == 0)
                SetStdHandle(STD_ERROR_HANDLE,
                             (HANDLE)_get_osfhandle(_fileno(stderr)));
        }
    }
#endif

    /* stdout unbuffered; stderr fully buffered, flushed every 500 ms.
     *
     * stderr used to be unbuffered, so every log line was its own WriteFile.
     * A sampler run put ~75% of the main thread's time in ZwWriteFile under
     * fprintf. On bad runs a redirected log managed ~4 lines a second, and the
     * boot didn't reach audio init in 25 minutes (the .rec scan alone logs
     * ~1,500 lines). One write per 1 MB or per 500 ms fixes that. The crash
     * handler and the watchdog both fflush(stderr) before exiting, so the
     * dumps that matter are not lost. RECOMP_STDERR_UNBUFFERED=1 restores
     * line-at-a-time output for a crash that kills the process outright. */
    setvbuf(stdout, NULL, _IONBF, 0);
    if (getenv("RECOMP_STDERR_UNBUFFERED"))
        setvbuf(stderr, NULL, _IONBF, 0);
    else {
        setvbuf(stderr, NULL, _IOFBF, 1 << 20);
        CloseHandle(CreateThread(NULL, 0, stderr_flusher, NULL, 0, NULL));
    }

    /* 1 ms timer resolution for the whole process. The helper threads that
     * stand in for hardware -- the GPU-idle shim (Sleep(1)), the kernel
     * timer thread that raises vblank (Sleep(10)) -- otherwise sleep in
     * 15.6 ms steps, and D3D's fence wait (sub_002E8110) spent a third of the
     * main thread's time waiting for the shim's next wake. (POSIX sleeps
     * are not quantised like that: nothing to do.) */
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif

    /* Raise the NV2A vblank interrupt (~60 Hz). D3D's ISR calls the title's
     * vertical-blank callback (0x000604A0, see recomp_manual.c), and the main
     * loop paces every frame on the counter it bumps -- without it the boot
     * stops at the intro movie. The runtime only checks that the variable
     * exists, so it is set here unless the environment already has it. */
    if (!getenv("RECOMP_VBLANK"))
        _putenv_s("RECOMP_VBLANK", "1");

    /* Playable by default: a double-clicked exe has no console and nobody
     * sets environment variables, and with these off it ran invisibly.
     *   RECOMP_PB_EXEC    execute the pushbuffer (software rasteriser)
     *   RECOMP_FB_WINDOW  show the framebuffer; closing it ends the process
     *   RECOMP_USB        the emulated USB gamepad (host XInput pad)
     *   RECOMP_KEYBOARD   keys on top of it: arrows, Enter=Start, Z/X/A/S
     * Each is set only when unset. RECOMP_HEADLESS=1 leaves all four alone,
     * for log-only and scripted runs. RECOMP_HEADLESS=0 means not headless:
     * the variable is removed here, because the runtime's modules only ask
     * whether it exists. (=0 turns RECOMP_FB_WINDOW off the same way.) */
    {
        const char *h = getenv("RECOMP_HEADLESS");
        if (h && *h == '0') {
#if defined(_WIN32)
            _putenv_s("RECOMP_HEADLESS", "");
#else
            unsetenv("RECOMP_HEADLESS");
#endif
        }
    }
    s_headless = getenv("RECOMP_HEADLESS") != NULL;

    /* Settings from blinx2_settings.ini next to the exe, edited in the
     * in-game menu (Escape). Not for headless runs: agents and scripts set
     * environment variables, and a variable always wins over the file. The
     * graphics options become the variables the runtime reads, when those
     * are unset; they take effect at the next start. */
    if (!s_headless) {
        /* The runtime's default is vulkan; "software" is the reference. */
        static const char *const gpu[] = { "vulkan", "software" };
        char about[512];
        xbox_settings_load("blinx2_settings.ini");
        xbox_setting_choice("graphics", "renderer", "Renderer", gpu, 2, 0,
                            "RECOMP_GPU", XBOX_SETTING_RESTART);
        xbox_setting_to_env("graphics", "renderer");
        xbox_setting_bool("graphics", "vk_present", "Present through Vulkan",
                          0, "RECOMP_VK_PRESENT", XBOX_SETTING_RESTART);
        xbox_setting_to_env("graphics", "vk_present");
        /* Borderless fullscreen from the start (F11 / Alt+Enter toggle it
         * any time); with Vulkan presentation, what frame-generating drivers
         * (NVIDIA Smooth Motion, AMD Fluid Motion Frames) work best with. */
        xbox_setting_bool("graphics", "fullscreen", "Start in fullscreen",
                          0, "RECOMP_FB_FULLSCREEN", XBOX_SETTING_RESTART);
        xbox_setting_to_env("graphics", "fullscreen");
        snprintf(about, sizeof about,
                 "Blinx 2: Masters of Time and Space -- static recompilation.\n"
                 "blinx2-recomp %s, xboxrecomp %s.\n"
                 "Settings: blinx2_settings.ini next to the exe.",
                 BLINX2_GIT_SHA, XBOXRECOMP_GIT_SHA);
        xbox_ui_set_about(about);
        cheats_menu_register();         /* the Cheats tab (cheats.c) */
    }
    if (!s_headless) {
        static const char *const on[] = {
            "RECOMP_PB_EXEC", "RECOMP_FB_WINDOW", "RECOMP_USB", "RECOMP_KEYBOARD"
        };
        int i;
        for (i = 0; i < 4; i++)
            if (!getenv(on[i]))
                _putenv_s(on[i], "1");
    }

    /* Sound: the emulated APU (RECOMP_AC97_READY: trap its registers, run the
     * voice processor, play through XAudio2). On by default in the playable
     * exe; RECOMP_AUDIO=0 turns it off. Headless runs stay silent unless they
     * set RECOMP_AUDIO=1 (or RECOMP_AC97_READY), so scripted runs keep their
     * timing and do not play sound over whatever the human is doing. */
    {
        const char *a = getenv("RECOMP_AUDIO");
        int want = a ? (*a != '0') : !s_headless;
        if (want && !getenv("RECOMP_AC97_READY"))
            _putenv_s("RECOMP_AC97_READY", "1");
    }

    /* Contiguous memory must be reclaimable. The title scene frees its
     * MmAllocateContiguousMemory blocks and the attract stage allocates new
     * ones; with the default bump allocator the 64 MB window runs out right
     * after the stg0101 cache copy, and the game shows its "dirty disc" error.
     * Same existence-only check as RECOMP_VBLANK. */
    if (!getenv("RECOMP_HEAP_RECLAIM"))
        _putenv_s("RECOMP_HEAP_RECLAIM", "1");

    /* Events the title builds itself must block. D3D's BlockUntilVerticalBlank
     * (sub_002E0DB0) zeroes SignalState of the KEVENT at device+0x1DBC by a
     * plain write and waits on it; without RECOMP_TITLE_KEVENTS the runtime
     * waits on the guest address as if it were a Win32 handle, which fails at
     * once. CRI's ADXM vsync thread (sub_002A1B20) waits there every pass, so
     * it ran ~80,000 times a second instead of 60, and Sofdec, which times
     * movies without an audio track by those vsyncs, played the title and
     * menu loops (title_movie_1a/2a.sfd, 6 s each, where the spinning crystal
     * is) about 17x too fast while spinning a host core. Movies with audio
     * sync to the audio and were unaffected. Set only when unset, like the two
     * above (the runtime wants it to be "1"); =0 turns it off. */
    if (!getenv("RECOMP_TITLE_KEVENTS"))
        _putenv_s("RECOMP_TITLE_KEVENTS", "1");

    /* The DirectSound GP-DSP doorbell is acked by a thread started later, once
     * guest memory exists; see dsp_doorbell_start(). */

    printf("=== Blinx 2 - Static Recompilation ===\n");
    printf("Loading XBE...\n");

    /* Install VEH handler (first handler in chain) */
    /* Load symbols up front rather than from inside the handler: at fault
     * time the process is already in a bad way, and SymInitialize
     * allocates. Failure is not fatal -- the handler prints no name. */
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
    SetUnhandledExceptionFilter(fatal_filter);
    atexit(exit_report_atexit);
    signal(SIGABRT, exit_report_sigabrt);

    /* Step 1: Load XBE */
    resolve_game_dir();
    fprintf(stderr, "[BOOT] game files: %s\n", BLINX_2_DIR);

    /* A network game (RECOMP_NET=host:<port> | join:<ip>:<port>): waits here
     * for the session -- the host hands its saves to every joiner, everyone
     * plays on a session copy with a fresh Z: cache -- then sets what every
     * peer must run alike (virtual mode, one pad per player, software
     * rendering for what the guest reads back) before anything reads those
     * settings (xboxrecomp src/net). */
    xbox_NetplaySetBuild("blinx2 " BLINX2_GIT_SHA " xboxrecomp " XBOXRECOMP_GIT_SHA
                         " gen " RECOMP_GEN_HASH);
    xbox_NetplayStart(BLINX_2_DIR);
    if (!load_xbe(BLINX_2_XBE_PATH, &xbe_data, &xbe_size)) {
        char msg[MAX_PATH + 256];
        snprintf(msg, sizeof msg,
                 "Failed to load %s.\n\n"
                 "Extract your own disc's files (default.xbe and the .ipk/.iph "
                 "archives) to a game_files folder next to this exe, or name "
                 "the folder that holds them: game_dir= under [paths] in "
                 "blinx2_settings.ini, or RECOMP_GAME_DIR.",
                 BLINX_2_XBE_PATH);
        MessageBoxA(NULL, msg, "Blinx 2 (recomp)", MB_ICONERROR);
        return 1;
    }
    printf("XBE loaded: %zu bytes\n", xbe_size);

    /* Step 2: Initialize Xbox memory layout */
    printf("Initializing Xbox memory layout...\n");
    /* Map 128 MB of guest address space while the title still sees 64 MB of
     * RAM. Blinx 2's image runs to 0x0303AC20, which leaves the runtime heap
     * only 14 MB (0x031C0000-0x04000000). The runtime also gives every guest
     * thread a 512 KB stack whatever the title asks for (the audio threads
     * ask for 12 KB). The heap ran dry starting the intro movie
     * ("xbox_HeapAlloc: out of memory (requested 65536, used
     * 14938388/14942208)"). A bigger mapping lets the heap run to 128 MB
     * without changing what the title is told about memory; see
     * xbox_SetMapSize in xbox_memory_layout.h. */
    xbox_SetMapSize(128u * 1024 * 1024);

    /* Contiguous arena above the image, in a 128 MB window. Device models
     * follow physical pointers, and with the arena starting at 0x1000 every
     * arena address below the image top (0x0303AC20) also names image
     * bytes: the OHCI model read the USB stack's control ED (physical
     * 0x0085C670) out of the image, saw an empty list, and the pad never
     * enumerated. RECOMP_CONTIG_LEGACY=1 restores 64 MB from 0x1000. */
    if (!getenv("RECOMP_CONTIG_LEGACY"))
        xbox_SetContigLayout(128u * 1024 * 1024, 1);

    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        MessageBoxA(NULL, "Failed to initialize Xbox memory layout.\n"
                    "The required virtual address range may be unavailable.",
                    "Recomp", MB_ICONERROR);
        free(xbe_data);
        return 1;
    }

    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped. Offset: 0x%llX\n", (unsigned long long)g_xbox_mem_offset);

    /* Controller input. The OHCI model traps the USB host-controller
     * apertures (0xFED00000 / 0xFED08000) and serves XAPI's register traffic
     * through the VEH above; it needs guest memory mapped, so it comes after
     * the layout. Nothing called it before, which left RECOMP_USB and
     * RECOMP_PAD_PRESS inert. Still opt-in: it does nothing unless
     * RECOMP_USB is set. */
    xbox_OhciInit();

    /* Bring up the emulated APU.
     *
     * Gated on the same variable that unmaps its registers, because the two
     * halves are useless apart: a trap with no model declines every access,
     * and a model nothing traps into never sees a register. The APU walks
     * Xbox physical RAM to find voice buffers, so it gets the guest RAM
     * base. */
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        fprintf(stderr, "[BOOT] emulated APU %s\n",
                g_apu_state ? "up" : "FAILED to initialise");
    }

    /* Step 3: Initialize Xbox kernel */
    printf("Initializing Xbox kernel replacement...\n");
    xbox_kernel_init();

    /* Step 4: Set game directory for file I/O path translation */
    {
        extern void xbox_path_init(const char *game_dir, const char *save_dir);
        /* RECOMP_SAVE_DIR moves saves and the Z: cache away from
         * %LOCALAPPDATA%\xboxrecomp, e.g. for a cold-cache test run that
         * must not touch the cache a running copy is using. */
        xbox_path_init(BLINX_2_DIR, getenv("RECOMP_SAVE_DIR"));
    }

    /* Step 5: Initialize kernel bridge (thunk table in Xbox memory) */
    printf("Initializing kernel bridge...\n");
    xbox_kernel_bridge_init();

    /* Step 6: Initialize stack */
    g_esp = XBOX_STACK_TOP;

    /*
     * TODO: Pre-initialize CRT globals if needed.
     *
     * Many Xbox games use the MSVC CRT. The CRT's __heap_init sets up a
     * heap descriptor at a game-specific address. You may need to
     * pre-initialize __active_heap to avoid small-block heap issues:
     *
     *   uint32_t *active_heap = (uint32_t *)((uint8_t *)g_xbox_mem_offset + ACTIVE_HEAP_VA);
     *   *active_heap = 1;  // 1 = system heap (HeapAlloc), avoids SBH init
     *
     * Find ACTIVE_HEAP_VA by searching the disassembly for __heap_init
     * or by looking for the CRT's __active_heap global in the data section.
     */

    printf("\n=== Initialization complete ===\n");
    printf("Entry point: 0x%08X\n", BLINX_2_ENTRY_POINT);
    printf("ESP: 0x%08X\n", g_esp);

    /* Build the flat dispatch table before the guest runs.
     *
     * Without this recomp_lookup falls back to a binary search over the
     * whole function table -- roughly log2(n) branches on *every*
     * indirect call, which for a 45,000-function C++ title is about 16
     * every time the game goes through a vtable. Half-Life 2 spent most
     * of its static initialisation inside recomp_lookup for exactly this
     * reason, and it read as a hang.
     *
     * Optional by design: if the allocation fails the search still works,
     * so a failure is worth one line and not a fatal error. */
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    /* Arm the hang watchdog. Does nothing unless RECOMP_WATCHDOG_SECS is set,
     * and must be called from this thread -- the guest registers it samples are
     * thread-local, so it has to be handed the copies belonging to the thread
     * that runs guest code.
     *
     * Not optional boilerplate: without this call RECOMP_WATCHDOG_SECS is
     * silently inert, and the one diagnostic that tells a hang from slowness
     * does nothing while appearing to be set. */
    xbox_WatchdogStart();
    sampler_start();
    memdump_start();
    dsp_doorbell_start();
    xbox_SetDpcSpinHook(dsound_timer_spin_hook);
    gpu_idle_start();

    /* After the XBE load, which titles the window from the certificate
     * ("BLiNX2"). */
    if (!s_headless)
#if defined(_WIN32)
        xbox_FramebufferWindowSetTitle((const uint16_t *)L"Blinx 2 (recomp)", 47);
#else
        xbox_FramebufferWindowSetTitle((const uint16_t *)u"Blinx 2 (recomp)", 47);
#endif

    /* Step 7: Call the recompiled entry point */
    printf("\nStarting game...\n");
    fflush(stdout);

    xbe_entry_point();

    printf("\nGame returned. Cleaning up...\n");

    /* Cleanup */
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);

    return 0;
}

/* ── XBE Loading ───────────────────────────────────────────── */

static BOOL load_xbe(const char *path, void **out_data, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open XBE: %s\n", path);
        return FALSE;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return FALSE;
    }

    void *data = malloc((size_t)size);
    if (!data) {
        fclose(f);
        return FALSE;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return FALSE;
    }

    fclose(f);
    *out_data = data;
    *out_size = (size_t)size;
    return TRUE;
}

/* Console entry point (for debugging -- lets you see printf output) */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if defined(_WIN32)
    return WinMain(GetModuleHandle(NULL), NULL, GetCommandLineA(), SW_SHOW);
#else
    return WinMain(NULL, NULL, NULL, 0);
#endif
}
