/* Shared internals of the src/ hooks (Hook A in dxhrfix_hooks.cpp, Hook B in
 * dxhrfix_precreate.cpp). Not part of the proxy interface. */
#ifndef DXHRFIX_INTERNAL_H
#define DXHRFIX_INTERNAL_H

#include "dxhrfix_hooks.h"

#include <windows.h>
#include <stddef.h>
#include <stdint.h>

#define FAA __attribute__((force_align_arg_pointer))
#define THISCALL __attribute__((thiscall))
#define BARRIER() __asm__ __volatile__("" ::: "memory")

static const uint32_t VA_BASE = 0x400000;
static const uint32_t VA_DEVICE_MANAGER = 0x12AB940;   // PCDX11DeviceManager*
static const uint32_t VA_RENDER_DEVICE10 = 0xEAAB48;   // PCDX11RenderDevice* + 0x10
static const uint32_t RING_SLOTS = 0x400;
static const uint32_t DM_CS = 0xA184;                  // CRITICAL_SECTION
static const uint32_t DM_D3D_DEVICE = 0x24;            // ID3D11Device*

struct AsyncItem {
    void **vtbl;                 // [0] asyncCreate(arg), thiscall, ret 4
    volatile uint32_t idx;       // queue index, 0x400 = claimed / none
    volatile uint8_t pending;
    volatile uint8_t requested;
};

typedef void (THISCALL *create_fn)(AsyncItem *, uint32_t);

extern dxhrfix_log_fn g_log;
extern int g_logLevel;
extern unsigned char *g_base;
extern void *volatile *g_dmPtr;
extern int64_t g_qpcFreq;

#define LOG(lvl, ...) do { if (g_log && (lvl) <= g_logLevel) g_log((lvl), __VA_ARGS__); } while (0)

static inline unsigned char *rebase(uint32_t va) { return g_base + (va - VA_BASE); }
static inline CRITICAL_SECTION *dm_cs(unsigned char *dm) { return (CRITICAL_SECTION *)(dm + DM_CS); }

static inline int64_t qpc(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static inline LONG ticks_to_us(int64_t d)
{
    if (d <= 0 || g_qpcFreq <= 0) return 0;
    int64_t us = d / g_qpcFreq * 1000000 + (d % g_qpcFreq) * 1000000 / g_qpcFreq;
    return us > 2000000000 ? 2000000000 : (LONG)us;
}

static inline LONG us_since(int64_t t0) { return ticks_to_us(qpc() - t0); }

void atomic_max(volatile LONG *dst, LONG v);
bool in_image_readable(uint32_t va, size_t n);

/* Trampoline hook: verify len bytes (abs = offsets of absolute dwords that get
 * the load delta added), relocate prologue bytes, jmp to detour. */
bool install_one(const char *name, uint32_t va, const uint8_t *orig, size_t len,
                 const int *abs, size_t nabs, size_t prologue, void *detour, void **tramp);

/* Whole-function replacement: verify all len bytes, write jmp detour + int3. */
bool replace_fn(const char *name, uint32_t va, const uint8_t *orig, size_t len,
                const int *abs, size_t nabs, void *detour);

/* Serialized request (Hook A). zero_d3d: also store 0 at item+0x18 after
 * winning the requested flag (what 0x5535F0 and 0x54C8D0 do before calling
 * request), so a shader created by another thread is never clobbered. */
int request_impl(AsyncItem *item, uint32_t arg, bool zero_d3d);

/* Hook B (dxhrfix_precreate.cpp). */
int precreate_install(const DxhrfixOptions *o);
void precreate_start_pool(void);
void precreate_report(void);

#endif
