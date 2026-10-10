/*
 * dxhrfix hooks (Hook A): await / cancelOrAwait detours and the frame latency
 * patch for DXHRDC.exe. See spec/SPEC.md sections 2, 3 and 5.
 *
 * dxhrfix_hooks_install must run once, single-threaded, before the game
 * creates its render and async-create worker threads (the proxy calls it from
 * DllMain PROCESS_ATTACH, which runs before the exe entry point).
 */
#ifndef DXHRFIX_HOOKS_H
#define DXHRFIX_HOOKS_H

typedef void (*dxhrfix_log_fn)(int level, const char *fmt, ...);

/* base = GetModuleHandle(NULL). frame_latency is clamped to 1..4.
 * Returns the number of patches applied (0..4). */
int dxhrfix_hooks_install(unsigned char *base, int frame_latency, int log_level,
                          dxhrfix_log_fn log);

/* Log the counters (level 1). final != 0 marks the process exit summary and
 * must be safe to call from DLL_PROCESS_DETACH. */
void dxhrfix_hooks_report(int final);

/* Start the 10 s totals reporter thread (once). Call outside DllMain, after
 * dxhrfix_hooks_install (the proxy calls it from the first GetDFEngine). */
void dxhrfix_hooks_start_reporter(void);

#endif
