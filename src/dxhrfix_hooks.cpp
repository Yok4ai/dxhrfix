// Hook A for DXHRDC.exe: serialized request, inline create on await,
// cancel-or-wait on destroy, and the SetMaximumFrameLatency immediate.
// Layouts and VAs: spec/SPEC.md.
//
// request 0x54C7D0 (thiscall ecx=item, 1 stack arg, ret 4) is replaced
// whole (its prologue holds a jne rel8, so no trampoline). Stock request sets
// pending before the queue index exists and stock enqueue 0x54A000 publishes
// ring slot and index outside the CS, after taking its slot number, so the
// filled semaphore can be released out of slot order. The worker can then
// claim a half-written slot (race R2: the awaiter later sees idx==slot and
// creates concurrently with the worker) or a stale one (race R1: orphaned
// item, dtor waits forever). The replacement sets idx=0x400 before
// pending=1, waits for a free slot outside the CS, then takes the slot
// number, publishes {item, arg}, writes idx and releases the filled
// semaphore all inside the device manager CS. Semaphore releases therefore
// happen in slot order and the worker (which claims under the same CS) never
// sees an unpublished slot. The lock-free "queue empty" test at 0x61E782
// (counter & 0x3FF == dequeue index) keeps its meaning.
//
// await 0x54C800 (thiscall ecx=item, plain ret): if item->pending, run the
// safe inline create protocol (spec section 2.4); when the worker owns the
// item, wait with SwitchToThread, then Sleep(0), then Sleep(1), re-running
// the locked slot check every few iterations (the item may be published
// after the first check).
//
// cancelOrAwait 0x54C850 (thiscall ecx=item, plain ret): destructors only.
// No inline create here: the object is being destroyed, so creating its D3D
// object only to release it right after is wasted work on the destroying
// thread. It cancels the queued slot under the CS (with the slot == item
// check the stock cancel 0x54A060 lacks) or waits for the worker.

#include "dxhrfix_hooks.h"
#include "../hooklib/hooklib.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>

#define FAA __attribute__((force_align_arg_pointer))
#define THISCALL __attribute__((thiscall))
#define BARRIER() __asm__ __volatile__("" ::: "memory")

// ---------------------------------------------------------------- engine layout

static const uint32_t VA_BASE = 0x400000;
static const uint32_t VA_DEVICE_MANAGER = 0x12AB940;   // PCDX11DeviceManager*
static const uint32_t VA_REQUEST = 0x54C7D0;
static const uint32_t VA_ENQUEUE = 0x54A000;
static const uint32_t VA_AWAIT = 0x54C800;
static const uint32_t VA_CANCEL_OR_AWAIT = 0x54C850;
static const uint32_t VA_LATENCY_CTX = 0x54AB80;       // 8b 51 30 6a 01 50 ff d2
static const uint32_t VA_LATENCY_IMM = 0x54AB84;       // the 01 of "6a 01"

static const uint32_t DM_RING = 0x180;       // {item*, arg} x 0x400
static const uint32_t DM_CS = 0xA184;        // CRITICAL_SECTION
static const uint32_t DM_ENQ_COUNT = 0xA19C; // LONG, InterlockedIncrement
static const uint32_t DM_SEM_FREE = 0xA1A4;
static const uint32_t DM_SEM_FILLED = 0xA1A8;
static const uint32_t RING_SLOTS = 0x400;

struct AsyncItem {
    void **vtbl;                 // [0] asyncCreate(arg), thiscall, ret 4
    volatile uint32_t idx;       // queue index, 0x400 = claimed / none
    volatile uint8_t pending;
    volatile uint8_t requested;
};

struct RingSlot {
    AsyncItem *volatile item;
    volatile uint32_t arg;
};

typedef void (THISCALL *create_fn)(AsyncItem *, uint32_t);
typedef int (THISCALL *fn0_t)(AsyncItem *);
typedef int (THISCALL *enqueue_fn)(void *dm, AsyncItem *, uint32_t);

// Original bytes. Dwords listed in the *_ABS arrays are absolute addresses
// and get (base - 0x400000) added before comparing.
static const uint8_t kRequestOrig[34] = {
    0x80, 0x79, 0x09, 0x00, 0x75, 0x19, 0xb0, 0x01, 0x88, 0x41, 0x09, 0x88, 0x41, 0x08, 0x8b, 0x44,
    0x24, 0x04, 0x50, 0x51, 0x8b, 0x0d, 0x40, 0xb9, 0x2a, 0x01, 0xe8, 0x11, 0xd8, 0xff, 0xff, 0xc2,
    0x04, 0x00};
static const int kRequestAbs[] = {22};
static const uint8_t kAwaitOrig[31] = {
    0x56, 0x8b, 0xf1, 0x8a, 0x46, 0x08, 0x84, 0xc0, 0x74, 0x13, 0x57, 0x8b, 0x3d, 0xa0, 0x12, 0xa8,
    0x00, 0x6a, 0x01, 0xff, 0xd7, 0x8a, 0x4e, 0x08, 0x84, 0xc9, 0x75, 0xf5, 0x5f, 0x5e, 0xc3};
static const int kAwaitAbs[] = {13};
static const uint8_t kCancelOrig[53] = {
    0x56, 0x8b, 0xf1, 0x80, 0x7e, 0x09, 0x00, 0x74, 0x2a, 0x8b, 0x0d, 0x40, 0xb9, 0x2a, 0x01, 0x56,
    0xe8, 0xfb, 0xd7, 0xff, 0xff, 0x84, 0xc0, 0x75, 0x1a, 0x8a, 0x46, 0x08, 0x84, 0xc0, 0x74, 0x13,
    0x57, 0x8b, 0x3d, 0xa0, 0x12, 0xa8, 0x00, 0x6a, 0x01, 0xff, 0xd7, 0x8a, 0x4e, 0x08, 0x84, 0xc9,
    0x75, 0xf5, 0x5f, 0x5e, 0xc3};
static const int kCancelAbs[] = {11, 35};
static const uint8_t kLatencyCtx[8] = {0x8b, 0x51, 0x30, 0x6a, 0x01, 0x50, 0xff, 0xd2};

// ---------------------------------------------------------------- state

static dxhrfix_log_fn g_log;
static int g_logLevel;
static unsigned char *g_base;
static void *volatile *g_dmPtr;
static fn0_t g_awaitTramp;
static fn0_t g_cancelTramp;
static enqueue_fn g_stockEnqueue;
static int64_t g_qpcFreq;            // ticks per second
static bool g_installed;

static volatile LONG g_requests;      // request: enqueued through the serialized path
static volatile LONG g_inline;        // await: created inline on the calling thread
static volatile LONG g_inlineLate;    // ...of which claimed by a re-check inside the wait loop
static volatile LONG g_fallback;      // await: worker owned it, waited
static volatile LONG g_awaitNoop;     // await: pending already clear on entry
static volatile LONG g_cancelled;     // cancelOrAwait: slot nulled, no wait
static volatile LONG g_dtorWaits;     // cancelOrAwait: worker owned it, waited
static volatile LONG g_dtorNoop;      // cancelOrAwait: nothing to do
static volatile LONG g_noDm;          // device manager null: stock path
static volatile LONG g_events;
static volatile LONG g_maxWaitUs;     // longest fallback wait (await + dtor)
static volatile LONG g_maxCreateUs;   // longest inline create
static volatile LONG g_reportedFinal;
static volatile LONG g_reporterStarted;

#define LOG(lvl, ...) do { if (g_log && (lvl) <= g_logLevel) g_log((lvl), __VA_ARGS__); } while (0)

static inline unsigned char *rebase(uint32_t va) { return g_base + (va - VA_BASE); }

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

static void atomic_max(volatile LONG *dst, LONG v)
{
    LONG cur = *dst;
    while (v > cur) {
        LONG prev = InterlockedCompareExchange(dst, v, cur);
        if (prev == cur) break;
        cur = prev;
    }
}

static void count_event(void)
{
    LONG n = InterlockedIncrement(&g_events);
    if (n % 1000 == 0)
        dxhrfix_hooks_report(0);
}

static inline CRITICAL_SECTION *dm_cs(unsigned char *dm) { return (CRITICAL_SECTION *)(dm + DM_CS); }
static inline RingSlot *dm_ring(unsigned char *dm) { return (RingSlot *)(dm + DM_RING); }

// Under the CS: if item still sits unclaimed in its slot, take it (null the
// slot, idx = 0x400) and return true with its arg.
static bool try_claim(unsigned char *dm, AsyncItem *item, uint32_t *slot, uint32_t *arg)
{
    RingSlot *ring = dm_ring(dm);
    bool ok = false;
    EnterCriticalSection(dm_cs(dm));
    uint32_t i = item->idx;
    *slot = i;
    if (i < RING_SLOTS && ring[i].item == item) {
        *arg = ring[i].arg;
        ring[i].item = NULL;            // worker skips a null slot
        item->idx = RING_SLOTS;         // claimed
        ok = true;
    }
    LeaveCriticalSection(dm_cs(dm));
    return ok;
}

static void create_inline(AsyncItem *item, uint32_t slot, uint32_t arg, bool late)
{
    int64_t t0 = qpc();
    ((create_fn)item->vtbl[0])(item, arg);
    item->pending = 0;
    LONG us = us_since(t0);
    atomic_max(&g_maxCreateUs, us);
    LONG n = InterlockedIncrement(&g_inline);
    if (late) InterlockedIncrement(&g_inlineLate);
    if (g_logLevel >= 2 || n <= 8)
        LOG(1, "await inline%s #%ld item %p vtbl %p slot 0x%lX arg 0x%lX create %ld us",
            late ? " (late)" : "", (long)n, item, item->vtbl, (unsigned long)slot,
            (unsigned long)arg, (long)us);
    count_event();
}

// Wait until the worker clears item->pending. If dm != NULL (await path),
// re-run the locked slot check every 16 iterations and create inline when it
// matches; *claimed is then set. Returns microseconds waited.
static LONG wait_pending(AsyncItem *item, const char *who, unsigned char *dm, bool *claimed)
{
    int64_t t0 = qpc();
    int64_t lastWarn = t0;
    if (claimed) *claimed = false;
    for (unsigned n = 0; item->pending; n++) {
        if (dm && (n & 15) == 15) {
            uint32_t slot, arg;
            if (try_claim(dm, item, &slot, &arg)) {
                LONG us = us_since(t0);
                create_inline(item, slot, arg, true);
                if (claimed) *claimed = true;
                return us;
            }
        }
        if (n < 64)
            SwitchToThread();
        else if (n < 256)
            Sleep(0);
        else {
            Sleep(1);
            if ((n & 255) == 0) {
                int64_t now = qpc();
                if (now - lastWarn >= g_qpcFreq) {
                    lastWarn = now;
                    LOG(1, "%s: still waiting on item %p (idx 0x%lX) after %ld ms", who, item,
                        (unsigned long)item->idx, (long)(us_since(t0) / 1000));
                }
            }
        }
    }
    LONG us = us_since(t0);
    atomic_max(&g_maxWaitUs, us);
    return us;
}

// ---------------------------------------------------------------- detours

static FAA int THISCALL request_detour(AsyncItem *item, uint32_t arg)
{
    if (item->requested)
        return 1;
    item->requested = 1;
    unsigned char *dm = (unsigned char *)*g_dmPtr;
    if (!dm) {                          // stock would fault in enqueue too
        InterlockedIncrement(&g_noDm);
        item->pending = 1;
        return g_stockEnqueue(dm, item, arg);
    }
    item->idx = RING_SLOTS;             // never garbage while pending
    BARRIER();
    item->pending = 1;
    BARRIER();
    // Free slot first, outside the CS: the worker needs the CS to free slots.
    WaitForSingleObject(*(HANDLE *)(dm + DM_SEM_FREE), INFINITE);
    EnterCriticalSection(dm_cs(dm));
    uint32_t i = (uint32_t)(InterlockedIncrement((volatile LONG *)(dm + DM_ENQ_COUNT)) - 1) &
                 (RING_SLOTS - 1);
    RingSlot *ring = dm_ring(dm);
    ring[i].item = item;
    ring[i].arg = arg;
    item->idx = i;
    ReleaseSemaphore(*(HANDLE *)(dm + DM_SEM_FILLED), 1, NULL);
    LeaveCriticalSection(dm_cs(dm));
    LONG n = InterlockedIncrement(&g_requests);
    if (g_logLevel >= 2)
        LOG(2, "request #%ld item %p vtbl %p slot 0x%lX arg 0x%lX", (long)n, item, item->vtbl,
            (unsigned long)i, (unsigned long)arg);
    return 1;
}

static FAA int THISCALL await_detour(AsyncItem *item)
{
    if (!item->pending) {               // all callers test pending first; cheap guard
        InterlockedIncrement(&g_awaitNoop);
        return 0;
    }
    unsigned char *dm = (unsigned char *)*g_dmPtr;
    if (!dm) {
        InterlockedIncrement(&g_noDm);
        return g_awaitTramp(item);
    }
    uint32_t slot, arg;
    if (try_claim(dm, item, &slot, &arg)) {
        create_inline(item, slot, arg, false);
        return 0;
    }
    bool claimed;
    LONG us = wait_pending(item, "await", dm, &claimed);
    if (claimed)
        return 0;
    LONG n = InterlockedIncrement(&g_fallback);
    if (g_logLevel >= 2 || n <= 8)
        LOG(1, "await fallback #%ld item %p idx 0x%lX waited %ld us", (long)n, item,
            (unsigned long)slot, (long)us);
    count_event();
    return 0;
}

static FAA int THISCALL cancel_detour(AsyncItem *item)
{
    if (!item->requested) {
        InterlockedIncrement(&g_dtorNoop);
        return 0;
    }
    unsigned char *dm = (unsigned char *)*g_dmPtr;
    if (!dm) {
        InterlockedIncrement(&g_noDm);
        return g_cancelTramp(item);
    }
    CRITICAL_SECTION *cs = dm_cs(dm);
    RingSlot *ring = dm_ring(dm);

    EnterCriticalSection(cs);
    uint32_t i = item->idx;
    if (i < RING_SLOTS && ring[i].item == item) {
        ring[i].item = NULL;            // cancelled, like stock 0x54A060
        item->idx = RING_SLOTS;
        LeaveCriticalSection(cs);
        LONG n = InterlockedIncrement(&g_cancelled);
        if (g_logLevel >= 2 || n <= 8)
            LOG(1, "dtor cancel #%ld item %p slot 0x%lX", (long)n, item, (unsigned long)i);
        count_event();
        return 0;
    }
    LeaveCriticalSection(cs);

    if (!item->pending) {
        InterlockedIncrement(&g_dtorNoop);
        return 0;
    }
    LONG us = wait_pending(item, "dtor", NULL, NULL);
    LONG n = InterlockedIncrement(&g_dtorWaits);
    if (g_logLevel >= 2 || n <= 8)
        LOG(1, "dtor wait #%ld item %p idx 0x%lX waited %ld us", (long)n, item, (unsigned long)i,
            (long)us);
    count_event();
    return 0;
}

// ---------------------------------------------------------------- install

static void hexs(const unsigned char *p, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 3] = d[p[i] >> 4];
        out[i * 3 + 1] = d[p[i] & 15];
        out[i * 3 + 2] = ' ';
    }
    out[n ? n * 3 - 1 : 0] = 0;
}

static bool in_image_readable(uint32_t va, size_t n)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_base + dos->e_lfanew);
    if (va < VA_BASE || va - VA_BASE + n > nt->OptionalHeader.SizeOfImage)
        return false;
    MEMORY_BASIC_INFORMATION mbi;
    const unsigned char *p = rebase(va);
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    return p + n <= (const unsigned char *)mbi.BaseAddress + mbi.RegionSize;
}

// Copy orig into out and add the load delta to each absolute dword.
static void rebased_expect(const uint8_t *orig, size_t len, const int *abs, size_t nabs,
                           uint8_t *out)
{
    memcpy(out, orig, len);
    uint32_t delta = (uint32_t)(uintptr_t)g_base - VA_BASE;
    for (size_t k = 0; k < nabs; k++) {
        uint32_t v;
        memcpy(&v, out + abs[k], 4);
        v += delta;
        memcpy(out + abs[k], &v, 4);
    }
}

static bool install_one(const char *name, uint32_t va, const uint8_t *orig, size_t len,
                        const int *abs, size_t nabs, size_t prologue, void *detour, fn0_t *tramp)
{
    char before[64], after[64], tb[64];
    uint8_t expect[64];
    if (!in_image_readable(va, len)) {
        LOG(0, "hook %s VA 0x%06lX: outside image or unreadable, SKIPPED", name, (unsigned long)va);
        return false;
    }
    rebased_expect(orig, len, abs, nabs, expect);
    unsigned char *t = rebase(va);
    hexs(t, 12, before);
    void *tr = NULL;
    int st = hook_install(t, expect, len, prologue, detour, &tr);
    if (st != HOOK_OK) {
        LOG(0, "hook %s VA 0x%06lX @%p FAILED: %s (bytes %s)", name, (unsigned long)va, t,
            hook_strerror(st), before);
        return false;
    }
    *tramp = (fn0_t)tr;
    hexs(t, 12, after);
    hexs((const unsigned char *)tr, prologue + 5, tb);
    LOG(0, "hook %s VA 0x%06lX @%p installed, verified %u bytes, prologue %u", name,
        (unsigned long)va, t, (unsigned)len, (unsigned)prologue);
    LOG(0, "  before: %s", before);
    LOG(0, "  after:  %s", after);
    LOG(0, "  detour %p, trampoline %p: %s", detour, tr, tb);
    return true;
}

// Whole-function replacement of request: jmp detour + int3 over all 34 bytes.
static bool replace_request(void)
{
    char before[128], after[128];
    const size_t len = sizeof(kRequestOrig);
    if (!in_image_readable(VA_REQUEST, len)) {
        LOG(0, "replace request: unreadable, SKIPPED");
        return false;
    }
    uint8_t expect[sizeof(kRequestOrig)], rep[sizeof(kRequestOrig)];
    rebased_expect(kRequestOrig, len, kRequestAbs, 1, expect);
    unsigned char *t = rebase(VA_REQUEST);
    hexs(t, len, before);
    memset(rep, 0xCC, len);
    rep[0] = 0xE9;
    int32_t rel = (int32_t)((unsigned char *)(void *)request_detour - (t + 5));
    memcpy(rep + 1, &rel, 4);
    int st = patch_bytes(t, expect, rep, len);
    if (st != HOOK_OK) {
        LOG(0, "replace request VA 0x%06lX @%p FAILED: %s (bytes %s)", (unsigned long)VA_REQUEST,
            t, hook_strerror(st), before);
        return false;
    }
    hexs(t, len, after);
    LOG(0, "replace request VA 0x%06lX @%p, verified %u bytes, detour %p (no trampoline)",
        (unsigned long)VA_REQUEST, t, (unsigned)len, (void *)request_detour);
    LOG(0, "  before: %s", before);
    LOG(0, "  after:  %s", after);
    return true;
}

static bool patch_latency(int latency)
{
    char before[32], after[32];
    if (!in_image_readable(VA_LATENCY_CTX, sizeof(kLatencyCtx))) {
        LOG(0, "frame latency: site unreadable, SKIPPED");
        return false;
    }
    unsigned char *ctx = rebase(VA_LATENCY_CTX);
    hexs(ctx, sizeof(kLatencyCtx), before);
    if (memcmp(ctx, kLatencyCtx, sizeof(kLatencyCtx)) != 0) {
        LOG(0, "frame latency: context mismatch at @%p (got %s), SKIPPED", ctx, before);
        return false;
    }
    if (latency == 1) {
        LOG(0, "frame latency: 1 = stock, not patched (%s)", before);
        return false;
    }
    const uint8_t want = 0x01, rep = (uint8_t)latency;
    int st = patch_bytes(rebase(VA_LATENCY_IMM), &want, &rep, 1);
    if (st != HOOK_OK) {
        LOG(0, "frame latency: patch_bytes failed: %s", hook_strerror(st));
        return false;
    }
    hexs(ctx, sizeof(kLatencyCtx), after);
    LOG(0, "frame latency VA 0x%06lX @%p patched 01 -> %02x (SetMaximumFrameLatency(%d))",
        (unsigned long)VA_LATENCY_IMM, rebase(VA_LATENCY_IMM), rep, latency);
    LOG(0, "  before: %s", before);
    LOG(0, "  after:  %s", after);
    return true;
}

int dxhrfix_hooks_install(unsigned char *base, int frame_latency, int log_level,
                          dxhrfix_log_fn log)
{
    g_base = base;
    g_log = log;
    g_logLevel = log_level;
    g_dmPtr = (void *volatile *)rebase(VA_DEVICE_MANAGER);
    g_stockEnqueue = (enqueue_fn)(void *)rebase(VA_ENQUEUE);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;
    if (frame_latency < 1) frame_latency = 1;
    if (frame_latency > 4) frame_latency = 4;

    int applied = 0;
    if (!in_image_readable(VA_DEVICE_MANAGER, 4)) {
        LOG(0, "hooks: device manager global VA 0x%06lX not in image, nothing patched",
            (unsigned long)VA_DEVICE_MANAGER);
        return 0;
    }
    // await first: request's replacement relies on the await hook to re-check
    // the ring, so only replace request when await is hooked as well.
    bool aw = install_one("await", VA_AWAIT, kAwaitOrig, sizeof(kAwaitOrig), kAwaitAbs, 1, 6,
                          (void *)await_detour, &g_awaitTramp);
    applied += aw;
    applied += install_one("cancelOrAwait", VA_CANCEL_OR_AWAIT, kCancelOrig, sizeof(kCancelOrig),
                           kCancelAbs, 2, 7, (void *)cancel_detour, &g_cancelTramp);
    if (aw)
        applied += replace_request();
    else
        LOG(0, "replace request: SKIPPED (await hook not installed)");
    applied += patch_latency(frame_latency);
    LOG(0, "hooks: %d/4 patches applied (device manager ptr @%p = %p)", applied, g_dmPtr,
        *g_dmPtr);
    g_installed = applied > 0;
    return applied;
}

void dxhrfix_hooks_report(int final)
{
    if (!g_log || !g_installed) return;
    if (final && InterlockedExchange(&g_reportedFinal, 1)) return;
    LOG(1, "%s: requests %ld, inline %ld (late %ld), fallback %ld, await_noop %ld, "
           "dtor_cancel %ld, dtor_wait %ld, dtor_noop %ld, no_dm %ld, max_wait %ld.%03ld ms, "
           "max_inline_create %ld.%03ld ms",
        final ? "totals (exit)" : "totals", (long)g_requests, (long)g_inline, (long)g_inlineLate,
        (long)g_fallback, (long)g_awaitNoop, (long)g_cancelled, (long)g_dtorWaits,
        (long)g_dtorNoop, (long)g_noDm, (long)(g_maxWaitUs / 1000), (long)(g_maxWaitUs % 1000),
        (long)(g_maxCreateUs / 1000), (long)(g_maxCreateUs % 1000));
}

static DWORD WINAPI reporter_thread(LPVOID)
{
    for (;;) {
        Sleep(10000);
        if (g_reportedFinal) return 0;
        dxhrfix_hooks_report(0);
    }
}

void dxhrfix_hooks_start_reporter(void)
{
    if (!g_installed || g_logLevel < 1) return;
    if (InterlockedExchange(&g_reporterStarted, 1)) return;
    HANDLE h = CreateThread(NULL, 0x10000, reporter_thread, NULL, 0, NULL);
    if (h) {
        CloseHandle(h);
        LOG(1, "reporter thread started (totals every 10 s)");
    } else {
        LOG(0, "reporter thread: CreateThread failed, err %lu", GetLastError());
    }
}
