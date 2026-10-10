/*
 * dxhrfix hooks for DXHRDC.exe. See spec/SPEC.md sections 2 to 5.
 *   Hook A: serialized request, inline create on await, cancel-or-wait on
 *           destroy, SetMaximumFrameLatency immediate.
 *   Hook B: precreate pixel shaders of each loaded shader library on a DLL
 *           thread pool.
 *
 * dxhrfix_hooks_install must run once, single-threaded, before the game
 * creates its render and async-create worker threads (the proxy calls it from
 * DllMain PROCESS_ATTACH, which runs before the exe entry point). It creates
 * no threads.
 */
#ifndef DXHRFIX_HOOKS_H
#define DXHRFIX_HOOKS_H

typedef void (*dxhrfix_log_fn)(int level, const char *fmt, ...);

struct DxhrfixOptions {
    int frame_latency;          /* clamped 1..4 */
    int log_level;
    int precreate;              /* 1 = Hook B on */
    int pool_threads;           /* 0 = auto: min(4, cores - 2), at least 1 */
    int precreate_max_per_sec;  /* 0 = unlimited */
    int cursor_fix;             /* 1 = dedupe ShowCursor / SetCursor / ClipCursor */
    int cursor_refresh_ms;      /* SetCursor / ClipCursor: repeat a real call at least this often */
};

/* base = GetModuleHandle(NULL). Returns the number of patches applied. */
int dxhrfix_hooks_install(unsigned char *base, const DxhrfixOptions *opt, dxhrfix_log_fn log);

/* Start the 10 s totals reporter and the precreate pool (once each). Call
 * outside DllMain; the proxy calls it from the first GetDFEngine. The pool
 * may also start earlier, lazily, on the first shader library load. */
void dxhrfix_hooks_start_threads(void);

/* Log the counters (level 1). Not safe from DLL_PROCESS_DETACH. */
void dxhrfix_hooks_report(void);

/* The totals / precreate lines the reporter logs, formatted into buf
 * (empty string when the hooks are not installed). Return the length. */
int dxhrfix_hooks_format_totals(char *buf, unsigned n);
int dxhrfix_hooks_format_precreate(char *buf, unsigned n);
int dxhrfix_hooks_format_cursor(char *buf, unsigned n);

/* Hitch profiler (src/dxhrfix_profiler.cpp). Independent of hooks=. */
struct DxhrfixProfilerOptions {
    int hz;                     /* samples per second per thread, 50..4000 */
    int hitch_ms;               /* frame time that triggers a dump */
    int max_dumps;              /* dumps per process */
    int ring_ms;                /* sample history kept, 100..4000 */
    int extra_threads;          /* 1 = also sample the other exe-created threads */
    int readfile;               /* 1 = count ReadFile calls (exe IAT hook) */
    int log_level;
    const wchar_t *out_dir;     /* dxhrfix_hitch.log goes here */
    const wchar_t *dll_dir;     /* dxhrfix_syms.tsv is read from here */
};

/* DllMain PROCESS_ATTACH, single-threaded, before the game threads exist:
 * records the main thread id, installs the Present hook (0x54C9F0) and the
 * ReadFile IAT hook. Creates no threads. Returns the patches applied; 0
 * means the profiler stays off. */
int dxhrfix_profiler_install(unsigned char *base, const DxhrfixProfilerOptions *opt,
                             dxhrfix_log_fn log);

/* Outside DllMain (first GetDFEngine): load symbols, open the hitch log,
 * start the sampler and writer threads. No-op when install failed. */
void dxhrfix_profiler_start(void);

#endif
