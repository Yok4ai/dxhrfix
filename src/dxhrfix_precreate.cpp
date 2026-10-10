// Hook B for DXHRDC.exe: precreate the pixel shaders of each shader table on
// a DLL thread pool, before first draw, so DXVK sees the shaders early.
// Layouts and VAs: spec/SPEC.md section 4.
//
// Sources of work: PixelShaderTable iteration 0x54F000 (state 2, called from
// the ShaderLib ctor 0x546F0E/0x546F21 and for the 5 builtin tables from
// 0x539850) and 0x54F080 (state 3, streamed ShaderLib::fill 0x5470B6/
// 0x5470C4). Both are thiscall ecx=table, plain ret. The detour runs the
// stock function first (its usage gate pushes nothing in the shipped game),
// then lists every shader of the table with d3d == 0, requested == 0,
// pending == 0. The create arg is 0: every stock PS request passes 0
// (0x5535F0 "push 0", 0x54C8D0 "push 0" at 0x54C8DD), and the PS create
// vfunc 0x54C8A0 ignores it.
//
// VS are not listed: every VS requests itself at construction (0x553590 ->
// 0x54C7B0 -> InternalResource vtbl[1] 0x553690 -> request(0)), so they are
// already queued to the engine worker by the time the table exists.
//
// Claim protocol (pool thread, under the DLL list CS, then the device
// manager CS): one 16-bit lock cmpxchg on {pending (+8), requested (+9)}
// from 0x0000 to 0x0101, then idx = 0x400. Readers that test requested then
// pending without the CS (setPS/setVS 0x54F890/0x54F9C0) therefore see both
// or neither. Losing the cmpxchg means the engine (request_impl, byte
// cmpxchg on requested) owns the shader: skip it. After the claim, an
// awaiter finds idx 0x400, fails its ring check and waits; cancelOrAwait in
// the dtor waits too. The create runs outside both locks, then pending = 0.
//
// Engine paths that zero d3d before request (0x5535F0 and 0x54C8D0) are
// replaced whole by versions that zero d3d only after winning the requested
// flag, so a shader the pool created is never clobbered.
//
// Lifetime: ~PixelShader (scalar deleting dtor 0x54C910, thiscall ecx=PS,
// arg flags, ret 4) is hooked; under the list CS it tombstones every list
// entry for that shader before the stock dtor runs. A pool thread that
// claimed it before is covered by pending=1 (the dtor's cancelOrAwait
// waits). Device manager shutdown 0x54A440 (thiscall ecx=dm, plain ret) is
// hooked: it stops the pool, clears the list and waits for in-flight creates.

#include "dxhrfix_internal.h"
#include "../hooklib/hooklib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t VA_QUEUE_USED = 0x54F000;
static const uint32_t VA_QUEUE_USED4 = 0x54F080;
static const uint32_t VA_PS_DTOR = 0x54C910;
static const uint32_t VA_DM_SHUTDOWN = 0x54A440;
static const uint32_t VA_PS_REQUEST = 0x5535F0;
static const uint32_t VA_PRECREATE_ONE = 0x54C8D0;

static const uint32_t TABLE_COUNT = 0xC;
static const uint32_t TABLE_SHADERS = 0x14;
static const uint32_t PS_D3D = 0x18;

static const uint8_t kQueueUsedOrig[16] = {0x51, 0xa1, 0x40, 0xb9, 0x2a, 0x01, 0x53, 0x55,
                                           0x8b, 0xa8, 0x50, 0x01, 0x00, 0x00, 0x57, 0x8b};
static const int kQueueUsedAbs[] = {2};
static const uint8_t kQueueUsed4Orig[16] = {0x83, 0xec, 0x0c, 0xa1, 0x40, 0xb9, 0x2a, 0x01,
                                            0x53, 0x8b, 0xd9, 0x8b, 0x88, 0x50, 0x01, 0x00};
static const int kQueueUsed4Abs[] = {4};
static const uint8_t kPsDtorOrig[13] = {0x56, 0x8b, 0xf1, 0x57, 0x8d, 0x7e, 0x0c,
                                        0xc7, 0x06, 0x6c, 0x8b, 0xa9, 0x00};
static const int kPsDtorAbs[] = {9};
static const uint8_t kDmShutdownOrig[16] = {0x56, 0x57, 0x8b, 0x3d, 0x84, 0x12, 0xa8, 0x00,
                                            0x8b, 0xf1, 0x8b, 0x86, 0xb4, 0xa1, 0x00, 0x00};
static const int kDmShutdownAbs[] = {4};
static const uint8_t kPsRequestOrig[15] = {0x6a, 0x00, 0xc7, 0x41, 0x18, 0x00, 0x00, 0x00,
                                           0x00, 0xe8, 0xd2, 0x91, 0xff, 0xff, 0xc3};
static const uint8_t kPrecreateOneOrig[35] = {
    0x83, 0x79, 0x18, 0x00, 0xc6, 0x05, 0x63, 0xb9, 0x2a, 0x01, 0x01, 0x75, 0x0e, 0x6a, 0x00, 0xc7,
    0x41, 0x18, 0x00, 0x00, 0x00, 0x00, 0xe8, 0xe5, 0xfe, 0xff, 0xff, 0xc6, 0x05, 0x63, 0xb9, 0x2a,
    0x01, 0x00, 0xc3};
static const int kPrecreateOneAbs[] = {6, 29};

typedef int (THISCALL *table_fn)(void *table);
typedef void *(THISCALL *dtor_fn)(AsyncItem *, uint32_t flags);
typedef int (THISCALL *dm_fn)(void *dm);

static table_fn g_queueUsedTramp, g_queueUsed4Tramp;
static dtor_fn g_psDtorTramp;
static dm_fn g_dmShutdownTramp;
static void *volatile *g_rdPtr;

// ---------------------------------------------------------------- work list

struct Entry {
    AsyncItem *item;   // NULL = tombstone
    LONG batch;
};

struct Batch {
    LONG id;           // -1 = free / finished
    void *table;       // logging only, never dereferenced after listing
    int kind;          // 2 = 0x54F000, 3 = 0x54F080
    LONG shaders, listed, created, lost, skipped, cleared;
    LONG createUs;
    int64_t t0;
};

static const int kBatches = 256;
static Batch g_batch[kBatches];
static LONG g_batchSeq;

static CRITICAL_SECTION g_listCs;
static Entry *g_list;
static LONG g_cap, g_head, g_tail;
static HANDLE g_workEvent;           // manual reset: set while the list may hold work

static int g_poolThreads;
static int g_maxPerSec;
static volatile LONG g_poolStarted;
static volatile LONG g_shutdown;
static volatile LONG g_inFlight;

static volatile LONG g_libs;         // table iterations seen (0x54F000 + 0x54F080)
static volatile LONG g_libsListed;   // ...with at least one shader listed
static volatile LONG g_listed;
static volatile LONG g_created;
static volatile LONG g_lost;         // requested already (engine or a duplicate entry) or d3d set
static volatile LONG g_dupSkipped;   // same shader twice in one table
static volatile LONG g_tablesRelisted; // table already listed (state 2/3 before the call)
static volatile LONG g_lostDone;     // ...of which d3d already set (created by the pool via a duplicate entry, or by the engine)
static volatile LONG g_skipped;      // tombstoned by ~PixelShader before a pool thread got it
static volatile LONG g_cleared;      // dropped at device shutdown / dm NULL
static volatile LONG g_waitNoDevice; // pool idled because the device was not up yet
static volatile LONG g_createUsTotal;
static volatile LONG g_maxCreateUs;

enum { ACC_CREATED, ACC_LOST, ACC_SKIPPED, ACC_CLEARED };

// Under g_listCs.
static void batch_account(LONG bid, int what, LONG us)
{
    Batch *b = &g_batch[bid & (kBatches - 1)];
    if (b->id != bid) return;
    switch (what) {
    case ACC_CREATED: b->created++; b->createUs += us; break;
    case ACC_LOST: b->lost++; break;
    case ACC_SKIPPED: b->skipped++; break;
    default: b->cleared++; break;
    }
    if (b->created + b->lost + b->skipped + b->cleared >= b->listed) {
        LONG wall = us_since(b->t0);
        LOG(1, "precreate lib #%ld table %p (%s): shaders %ld, listed %ld, created %ld, lost %ld, "
               "skipped_unload %ld, cleared %ld, create %ld.%03ld ms, wall %ld.%03ld ms",
            (long)bid, b->table, b->kind == 2 ? "0x54F000" : "0x54F080", (long)b->shaders,
            (long)b->listed,
            (long)b->created, (long)b->lost, (long)b->skipped, (long)b->cleared,
            (long)(b->createUs / 1000), (long)(b->createUs % 1000), (long)(wall / 1000),
            (long)(wall % 1000));
        b->id = -1;
    }
}

// Under g_listCs.
static bool list_append(AsyncItem *item, LONG bid)
{
    if (g_tail == g_cap) {
        if (g_head > 0) {
            memmove(g_list, g_list + g_head, (size_t)(g_tail - g_head) * sizeof(Entry));
            g_tail -= g_head;
            g_head = 0;
        }
        if (g_tail == g_cap) {
            LONG ncap = g_cap ? g_cap * 2 : 1024;
            Entry *n = (Entry *)realloc(g_list, (size_t)ncap * sizeof(Entry));
            if (!n) return false;
            g_list = n;
            g_cap = ncap;
        }
    }
    g_list[g_tail].item = item;
    g_list[g_tail].batch = bid;
    g_tail++;
    return true;
}

// Under g_listCs.
static void list_clear(void)
{
    for (LONG i = g_head; i < g_tail; i++) {
        if (g_list[i].item) {
            g_list[i].item = NULL;
            InterlockedIncrement(&g_cleared);
            batch_account(g_list[i].batch, ACC_CLEARED, 0);
        }
    }
    g_head = g_tail = 0;
    ResetEvent(g_workEvent);
}

static void list_table(void *table, int kind)
{
    InterlockedIncrement(&g_libs);
    if (g_shutdown || !table) return;
    uint32_t count = *(uint32_t *)((unsigned char *)table + TABLE_COUNT);
    AsyncItem **arr = *(AsyncItem ***)((unsigned char *)table + TABLE_SHADERS);
    if (!arr || !count) return;

    EnterCriticalSection(&g_listCs);
    LONG bid = InterlockedIncrement(&g_batchSeq);
    Batch *b = &g_batch[bid & (kBatches - 1)];   // claimed below only if listed > 0
    LONG listed = 0;
    for (uint32_t i = 0; i < count; i++) {
        AsyncItem *s = arr[i];
        if (!s || s->requested || s->pending ||
            *(void *volatile *)((unsigned char *)s + PS_D3D))
            continue;
        // This batch's entries are the last `listed` ones (appended under
        // this CS hold, list_append keeps them contiguous): skip duplicates.
        bool dup = false;
        for (LONG k = g_tail - listed; k < g_tail; k++)
            if (g_list[k].item == s) { dup = true; break; }
        if (dup) {
            InterlockedIncrement(&g_dupSkipped);
            continue;
        }
        if (!list_append(s, bid)) break;
        listed++;
    }
    if (listed) {
        b->table = table;
        b->kind = kind;
        b->listed = listed;
        b->shaders = (LONG)count;
        b->created = b->lost = b->skipped = b->cleared = 0;
        b->createUs = 0;
        b->t0 = qpc();
        b->id = bid;
        InterlockedIncrement(&g_libsListed);
        InterlockedExchangeAdd(&g_listed, listed);
        SetEvent(g_workEvent);
    }
    LeaveCriticalSection(&g_listCs);
    if (listed)
        LOG(2, "precreate lib #%ld table %p (%s): %lu shaders, %ld listed, queue %ld",
            (long)bid, table, kind == 2 ? "0x54F000" : "0x54F080", (unsigned long)count,
            (long)listed, (long)(g_tail - g_head));
    else
        LOG(2, "precreate table %p (%s): %lu shaders, none to list", table,
            kind == 2 ? "0x54F000" : "0x54F080", (unsigned long)count);
    precreate_start_pool();   // lazy; never runs in DllMain
}

// ---------------------------------------------------------------- pool

static DWORD WINAPI pool_thread(LPVOID)
{
    const DWORD pause = g_maxPerSec > 0 ? (DWORD)(g_poolThreads * 1000 / g_maxPerSec) : 0;
    for (;;) {
        WaitForSingleObject(g_workEvent, INFINITE);

        // Shutdown flag, dm and device are read only here, under the list
        // CS: the shutdown hook (0x54A440, which runs before the dm is freed
        // at 0x54C221) sets the flag and then takes this CS.
        EnterCriticalSection(&g_listCs);
        if (g_shutdown) {
            list_clear();
            LeaveCriticalSection(&g_listCs);
            Sleep(100);
            continue;
        }
        unsigned char *dm = (unsigned char *)*g_dmPtr;
        if (!*g_rdPtr || !dm || !*(void *volatile *)(dm + DM_D3D_DEVICE)) {
            LeaveCriticalSection(&g_listCs);
            InterlockedIncrement(&g_waitNoDevice);
            Sleep(50);       // device not up yet: keep the work, retry
            continue;
        }
        while (g_head < g_tail && !g_list[g_head].item)
            g_head++;
        if (g_head == g_tail) {
            g_head = g_tail = 0;
            ResetEvent(g_workEvent);
            LeaveCriticalSection(&g_listCs);
            continue;
        }
        Entry e = g_list[g_head++];
        bool won = false;
        if (!*(void *volatile *)((unsigned char *)e.item + PS_D3D)) {
            EnterCriticalSection(dm_cs(dm));
            won = __sync_bool_compare_and_swap((volatile uint16_t *)&e.item->pending,
                                               (uint16_t)0, (uint16_t)0x0101);
            if (won)
                e.item->idx = RING_SLOTS;
            LeaveCriticalSection(dm_cs(dm));
        }
        if (won) {
            InterlockedIncrement(&g_inFlight);
        } else {
            InterlockedIncrement(&g_lost);
            if (*(void *volatile *)((unsigned char *)e.item + PS_D3D))
                InterlockedIncrement(&g_lostDone);
            batch_account(e.batch, ACC_LOST, 0);
        }
        LeaveCriticalSection(&g_listCs);
        if (!won)
            continue;

        int64_t t0 = qpc();
        ((create_fn)e.item->vtbl[0])(e.item, 0);
        BARRIER();
        e.item->pending = 0;          // the item may die right after this store
        BARRIER();
        LONG us = us_since(t0);
        InterlockedDecrement(&g_inFlight);
        InterlockedIncrement(&g_created);
        InterlockedExchangeAdd(&g_createUsTotal, us);
        atomic_max(&g_maxCreateUs, us);
        EnterCriticalSection(&g_listCs);
        batch_account(e.batch, ACC_CREATED, us);
        LeaveCriticalSection(&g_listCs);
        if (pause)
            Sleep(pause);
    }
}

void precreate_start_pool(void)
{
    if (!g_workEvent || InterlockedExchange(&g_poolStarted, 1)) return;
    int started = 0;
    for (int i = 0; i < g_poolThreads; i++) {
        HANDLE h = CreateThread(NULL, 0x40000, pool_thread, NULL, 0, NULL);
        if (!h) {
            LOG(0, "precreate pool: CreateThread failed, err %lu", GetLastError());
            break;
        }
        CloseHandle(h);
        started++;
    }
    LOG(1, "precreate pool: %d threads started (normal priority), max_per_sec %d",
        started, g_maxPerSec);
}

// ---------------------------------------------------------------- detours

// Table state +0x1C: ctor 0x54EE20 sets 0, 0x54EFA0 sets 1, 0x54F000 sets
// 2, 0x54F080 sets 3. A table that already went through 0x54F000 or
// 0x54F080 (state 2 or 3 before the call) was listed then: skip it.
static const uint32_t TABLE_STATE = 0x1C;

static FAA int THISCALL queue_used_detour(void *table)
{
    uint32_t pre = *(volatile uint32_t *)((unsigned char *)table + TABLE_STATE);
    int r = g_queueUsedTramp(table);
    if (pre == 2 || pre == 3)
        InterlockedIncrement(&g_tablesRelisted);
    else
        list_table(table, 2);
    return r;
}

static FAA int THISCALL queue_used4_detour(void *table)
{
    uint32_t pre = *(volatile uint32_t *)((unsigned char *)table + TABLE_STATE);
    int r = g_queueUsed4Tramp(table);
    if (pre == 2 || pre == 3)
        InterlockedIncrement(&g_tablesRelisted);
    else
        list_table(table, 3);
    return r;
}

static FAA void *THISCALL ps_dtor_detour(AsyncItem *item, uint32_t flags)
{
    EnterCriticalSection(&g_listCs);
    for (LONG i = g_head; i < g_tail; i++) {
        if (g_list[i].item == item) {
            g_list[i].item = NULL;
            InterlockedIncrement(&g_skipped);
            batch_account(g_list[i].batch, ACC_SKIPPED, 0);
        }
    }
    LeaveCriticalSection(&g_listCs);
    return g_psDtorTramp(item, flags);
}

static FAA int THISCALL dm_shutdown_detour(void *dm)
{
    InterlockedExchange(&g_shutdown, 1);
    EnterCriticalSection(&g_listCs);
    list_clear();
    LeaveCriticalSection(&g_listCs);
    int64_t t0 = qpc();
    while (g_inFlight > 0 && us_since(t0) < 5000000)
        Sleep(1);
    LOG(1, "precreate: device manager shutdown, list cleared, in-flight %ld after %ld us",
        (long)g_inFlight, (long)us_since(t0));
    return g_dmShutdownTramp(dm);
}

// 0x5535F0 (PS/VS requestShader, thiscall ecx=shader, no args, plain ret):
// stock does d3d = 0; request(0).
static FAA int THISCALL ps_request_detour(AsyncItem *shader)
{
    return request_impl(shader, 0, true);
}

// 0x54C8D0 (precreateOne, thiscall ecx=PS, no args, plain ret): stock does
// if (!d3d) { d3d = 0; request(0); } around writes of the never-read byte
// 0x12AB963 (dropped here).
static FAA int THISCALL precreate_one_detour(AsyncItem *shader)
{
    if (*(void *volatile *)((unsigned char *)shader + PS_D3D))
        return 0;
    return request_impl(shader, 0, true);
}

// ---------------------------------------------------------------- install

int precreate_install(const DxhrfixOptions *o)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int cores = (int)si.dwNumberOfProcessors;
    int n = o->pool_threads;
    if (n <= 0) {
        n = cores - 2;
        if (n > 4) n = 4;
        if (n < 1) n = 1;
    }
    if (n > 8) n = 8;
    g_poolThreads = n;
    g_maxPerSec = o->precreate_max_per_sec > 0 ? o->precreate_max_per_sec : 0;
    g_rdPtr = (void *volatile *)rebase(VA_RENDER_DEVICE10);
    InitializeCriticalSection(&g_listCs);
    g_workEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < kBatches; i++)
        g_batch[i].id = -1;
    if (!g_workEvent) {
        LOG(0, "precreate: CreateEvent failed, Hook B not installed");
        return 0;
    }

    // Lifetime hooks first; the list sources only when both are in place.
    bool dt = install_one("~PixelShader", VA_PS_DTOR, kPsDtorOrig, sizeof(kPsDtorOrig), kPsDtorAbs,
                          1, 7, (void *)ps_dtor_detour, (void **)&g_psDtorTramp);
    bool sd = install_one("dmShutdown", VA_DM_SHUTDOWN, kDmShutdownOrig, sizeof(kDmShutdownOrig),
                          kDmShutdownAbs, 1, 8, (void *)dm_shutdown_detour,
                          (void **)&g_dmShutdownTramp);
    bool r1 = replace_fn("psRequest", VA_PS_REQUEST, kPsRequestOrig, sizeof(kPsRequestOrig), NULL,
                         0, (void *)ps_request_detour);
    bool r2 = replace_fn("precreateOne", VA_PRECREATE_ONE, kPrecreateOneOrig,
                         sizeof(kPrecreateOneOrig), kPrecreateOneAbs, 2,
                         (void *)precreate_one_detour);
    int applied = dt + sd + r1 + r2;
    if (!(dt && sd && r1 && r2)) {
        LOG(0, "precreate: lifetime/request hooks incomplete, list sources not hooked");
        return applied;
    }
    applied += install_one("queueUsed", VA_QUEUE_USED, kQueueUsedOrig, sizeof(kQueueUsedOrig),
                           kQueueUsedAbs, 1, 6, (void *)queue_used_detour,
                           (void **)&g_queueUsedTramp);
    applied += install_one("queueUsed4", VA_QUEUE_USED4, kQueueUsed4Orig, sizeof(kQueueUsed4Orig),
                           kQueueUsed4Abs, 1, 8, (void *)queue_used4_detour,
                           (void **)&g_queueUsed4Tramp);
    LOG(0, "precreate: %d Hook B patches, pool %d threads (cores %d), max_per_sec %d", applied,
        g_poolThreads, cores, g_maxPerSec);
    return applied;
}

int precreate_format(char *buf, size_t n)
{
    if (!n) return 0;
    LONG queued = 0;
    EnterCriticalSection(&g_listCs);
    for (LONG i = g_head; i < g_tail; i++)
        queued += g_list[i].item != NULL;
    LeaveCriticalSection(&g_listCs);
    int k = _snprintf(buf, n - 1,
        "precreate: libs %ld (listed %ld, relisted skipped %ld), shaders listed %ld (dup skipped %ld), created %ld, lost %ld (already created %ld), "
        "skipped_unload %ld, cleared %ld, queued %ld, in_flight %ld, no_device_waits %ld, "
        "create_total %ld.%03ld ms, max_create %ld.%03ld ms",
        (long)g_libs, (long)g_libsListed, (long)g_tablesRelisted, (long)g_listed,
        (long)g_dupSkipped, (long)g_created, (long)g_lost, (long)g_lostDone,
        (long)g_skipped, (long)g_cleared, (long)queued, (long)g_inFlight, (long)g_waitNoDevice,
        (long)(g_createUsTotal / 1000), (long)(g_createUsTotal % 1000),
        (long)(g_maxCreateUs / 1000), (long)(g_maxCreateUs % 1000));
    if (k < 0) k = (int)n - 1;
    buf[k] = 0;
    return k;
}
