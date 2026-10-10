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
};

/* base = GetModuleHandle(NULL). Returns the number of patches applied. */
int dxhrfix_hooks_install(unsigned char *base, const DxhrfixOptions *opt, dxhrfix_log_fn log);

/* Start the 10 s totals reporter and the precreate pool (once each). Call
 * outside DllMain; the proxy calls it from the first GetDFEngine. The pool
 * may also start earlier, lazily, on the first shader library load. */
void dxhrfix_hooks_start_threads(void);

/* Log the counters (level 1). Not safe from DLL_PROCESS_DETACH. */
void dxhrfix_hooks_report(void);

#endif
