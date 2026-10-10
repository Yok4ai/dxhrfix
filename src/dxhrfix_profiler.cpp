// Sampling hitch profiler for DXHRDC.exe.
//
// Frame boundary: the engine's Present wrapper 0x54C9F0 (thiscall
// ecx=RenderContext, 3 stack args, ret 0xC, called only from 0x53A584 in the
// render frame function 0x53A170) is hooked with a trampoline. The time
// between two successive Present returns on the render thread is the frame
// time. The first thread that calls it is the render thread.
//
// Threads: main = the thread that ran DllMain PROCESS_ATTACH (static import,
// so the process's initial thread). The others are found from a Toolhelp
// thread snapshot by their Win32 start address (NtQueryInformationThread
// class 9): render 0x53BAD0, async create worker 0x54A100, threaded file
// system worker 0x4A2E10 (loop 0x49FB40), plus with profiler_extra_threads=1
// the other exe CreateThread entries. Fallbacks: render = the Present
// caller, async worker = GetThreadId of the handle at deviceManager+0xA180.
//
// Sampler: one TIME_CRITICAL thread. Each tick, for every tracked thread:
// SuspendThread, GetThreadContext, conservative stack scan (prof_scan_stack:
// up to 4096 bytes from Esp, dwords inside an executable section that follow
// a call instruction), ResumeThread. Inside that suspended window it calls
// nothing but those three kernel32 functions and plain memory reads: no
// heap, no locks, no logging, no loader calls. Module code ranges are
// rebuilt every 2 s from K32EnumProcessModules while nothing is suspended,
// and every listed module holds a reference (GetModuleHandleExW) so it
// cannot unload while its range is in use.
//
// Dumps: when a frame exceeds profiler_hitch_ms, the Present detour pushes
// {prev present, this present} into a small single producer queue. The
// sampler copies the samples of that window into a snapshot buffer (after
// the tick that follows the window end) and wakes the writer thread, which
// aggregates, symbolizes and appends to dxhrfix_hitch.log. Neither the
// sampler nor the writer ever blocks the render thread.
//
// ReadFile: the exe's IAT slot for KERNEL32!ReadFile (0xA81060) is pointed
// at a counter that records {thread, bytes, start, end} into a lock free
// ring; each dump summarizes the calls that overlap its window.
//
// Lock order: g_modCs is a leaf lock. The sampler takes it only to swap the
// module table (GetModuleHandleExW / FreeLibrary run outside it), the writer
// and the sampler take it around each symbol lookup. Nothing else is
// acquired while it is held. No game hook path takes it.

#include "dxhrfix_internal.h"
#include "dxhrfix_prof_core.h"
#include "../hooklib/hooklib.h"

#include <tlhelp32.h>
#include <psapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- engine layout

static const uint32_t VA_PRESENT = 0x54C9F0;
static const uint32_t VA_RENDER_THREAD = 0x53BAD0;
static const uint32_t VA_ASYNC_WORKER = 0x54A100;
static const uint32_t VA_FS_THREAD = 0x4A2E10;
static const uint32_t VA_READFILE_IAT = 0xA81060;
static const uint32_t DM_WORKER_HANDLE = 0xA180;

// Other CreateThread entries in the exe (spec section 6), sampled only with
// profiler_extra_threads=1. 0xA39574 is the CRT _beginthreadex trampoline.
static const uint32_t kExtraStarts[] = {0x452290, 0x4A1190, 0x5B12E0, 0x6AE390,
                                        0x6AE670, 0xA09CB0, 0xA39574};

// 56 | 8b f1 | 8b 0d [deviceManager] | 8b 01 | 8b 50 04 | ff d2 | 80 78 18 00 | a1 [deviceManager]
static const uint8_t kPresentOrig[25] = {
    0x56, 0x8b, 0xf1, 0x8b, 0x0d, 0x40, 0xb9, 0x2a, 0x01, 0x8b, 0x01, 0x8b, 0x50,
    0x04, 0xff, 0xd2, 0x80, 0x78, 0x18, 0x00, 0xa1, 0x40, 0xb9, 0x2a, 0x01};
static const int kPresentAbs[] = {5, 21};

// ---------------------------------------------------------------- limits

enum { MAX_SLOTS = 16, MAX_RA = 24, MAX_MODS = 256, MAX_RANGES = 512, REQ_N = 16,
       RF_N = 16384, IGNORE_N = 512, STACK_SCAN_BYTES = 4096 };

enum Role { ROLE_NONE = -1, ROLE_MAIN, ROLE_RENDER, ROLE_ASYNC, ROLE_FS, ROLE_EXTRA };
static const char *const kRoleName[] = {"main", "render", "async_create", "fs_worker", "extra"};

struct Slot {
    DWORD tid;
    HANDLE h;
    int role;
    uint32_t startVa;          // preferred base VA, 0 = unknown
    const uint8_t *teb;
    volatile LONG active;
    volatile LONG samples, fails, noStack;
    int64_t suspTicks, suspMax;   // SuspendThread call .. ResumeThread return
};

struct Sample {
    int64_t t;
    uint16_t slot;
    uint16_t n;
    uint32_t eip;
    uint32_t ra[MAX_RA];
};

struct HitchReq {
    int64_t t0, t1;
    LONG frame;
};

struct RfEvent {
    volatile uint32_t seq;     // index + 1 once complete, 0 while written
    DWORD tid;
    uint32_t req;              // bytes requested; bit 31 = overlapped
    uint32_t got;              // bytes read (0 for overlapped)
    int64_t t0, t1;
};

struct Mod {
    uint32_t base, size;
    HMODULE ref;
    char name[48];
};

typedef LONG (WINAPI *NtQIT_fn)(HANDLE, int, void *, ULONG, ULONG *);
typedef int (THISCALL *present_fn)(void *ctx, uint32_t, uint32_t, uint32_t);
typedef BOOL (WINAPI *ReadFile_fn)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);

// ---------------------------------------------------------------- state

static bool g_profInstalled;
static DxhrfixProfilerOptions g_po;
static wchar_t g_outDir[MAX_PATH];
static wchar_t g_dllDir[MAX_PATH];
static DWORD g_mainTid;
static uint32_t g_imageSize;

// Present / frames (written by the render thread only)
static present_fn g_presentTramp;
static volatile LONG g_renderTid;
static volatile LONG g_samplerRunning;
static int64_t g_hitchTicks;
static int64_t g_lastPresent;
static volatile LONG g_frames;
static volatile LONG g_hitches;
static volatile LONG g_maxFrameUs;
static HitchReq g_req[REQ_N];
static volatile LONG g_reqHead, g_reqTail;
static volatile LONG g_dumpsQueued, g_reqDropped;

// ReadFile
static ReadFile_fn g_realReadFile;
static RfEvent g_rf[RF_N];
static volatile LONG g_rfHead;
static volatile LONG g_rfCalls;

// Sampler
static Slot g_slots[MAX_SLOTS];
static volatile LONG g_nslots;
static Sample *g_ring, *g_snap;
static uint32_t g_ringCap;
static volatile uint32_t g_ringHead;   // samples ever written
static CodeRange g_ranges[MAX_RANGES]; // sampler thread only
static int g_nranges;
static DWORD g_ignore[IGNORE_N];
static int g_nignore;
static NtQIT_fn g_ntqit;
static bool g_startAddrWorks;
static int64_t g_period;
static volatile LONG g_ticks;
static int64_t g_tickTotal, g_tickMax;
static DWORD g_samplerTid, g_writerTid;

// Modules (g_modCs)
static CRITICAL_SECTION g_modCs;
static Mod g_mods[MAX_MODS];
static int g_nmods;

// Writer
static HANDLE g_writerEvent;
static volatile LONG g_writerBusy;
static HitchReq g_snapReq;
static uint32_t g_snapN;
static bool g_snapTruncated;
static HANDLE g_hitchLog = INVALID_HANDLE_VALUE;
static volatile LONG g_dumpsWritten;
static SymTable g_syms;
static char *g_symBuf;
static int64_t g_qpc0;
static uint64_t g_wall0;               // FILETIME at g_qpc0

// ---------------------------------------------------------------- Present hook

static volatile LONG g_startKick;

static DWORD WINAPI start_kick_thread(LPVOID)
{
    dxhrfix_profiler_start();
    return 0;
}

static FAA int THISCALL present_detour(void *ctx, uint32_t a, uint32_t b, uint32_t c)
{
    // First frame: start the sampler from a helper thread (the proxy's first
    // GetDFEngine, the other start point, comes about 25 s into startup).
    if (!g_startKick && InterlockedExchange(&g_startKick, 1) == 0) {
        HANDLE h = CreateThread(NULL, 0x10000, start_kick_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
    }
    int r = g_presentTramp(ctx, a, b, c);
    int64_t now = qpc();
    LONG tid = (LONG)GetCurrentThreadId();
    if (!g_renderTid)
        InterlockedCompareExchange(&g_renderTid, tid, 0);
    if (tid != g_renderTid)
        return r;
    LONG f = InterlockedIncrement(&g_frames);
    int64_t last = g_lastPresent;
    g_lastPresent = now;
    if (!last)
        return r;
    int64_t d = now - last;
    LONG us = ticks_to_us(d);
    if (us > g_maxFrameUs)
        g_maxFrameUs = us;
    if (d < g_hitchTicks)
        return r;
    InterlockedIncrement(&g_hitches);
    if (!g_samplerRunning || g_dumpsQueued >= g_po.max_dumps)
        return r;
    LONG h = g_reqHead;
    if (h - g_reqTail >= REQ_N) {
        InterlockedIncrement(&g_reqDropped);
        return r;
    }
    HitchReq *q = &g_req[h & (REQ_N - 1)];
    q->t0 = last;
    q->t1 = now;
    q->frame = f;
    BARRIER();
    g_reqHead = h + 1;
    InterlockedIncrement(&g_dumpsQueued);
    return r;
}

// ---------------------------------------------------------------- ReadFile hook

static FAA BOOL WINAPI readfile_detour(HANDLE h, LPVOID buf, DWORD n, LPDWORD got,
                                       LPOVERLAPPED ov)
{
    int64_t t0 = qpc();
    BOOL ok = g_realReadFile(h, buf, n, got, ov);
    DWORD err = GetLastError();
    int64_t t1 = qpc();
    uint32_t idx = (uint32_t)InterlockedIncrement(&g_rfHead) - 1;
    RfEvent *e = &g_rf[idx & (RF_N - 1)];
    e->seq = 0;
    BARRIER();
    e->tid = GetCurrentThreadId();
    e->req = (n & 0x7FFFFFFF) | (ov ? 0x80000000u : 0);
    e->got = (ok && got && !ov) ? *got : 0;
    e->t0 = t0;
    e->t1 = t1;
    BARRIER();
    e->seq = idx + 1;
    InterlockedIncrement(&g_rfCalls);
    SetLastError(err);
    return ok;
}

// Find the IAT slot of dll!func in the exe import directory.
static void **find_iat_slot(unsigned char *base, const char *dll, const char *func)
{
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress || !dd.Size)
        return NULL;
    IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress);
    for (; d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll) != 0)
            continue;
        if (!d->OriginalFirstThunk)
            continue;
        IMAGE_THUNK_DATA32 *names = (IMAGE_THUNK_DATA32 *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32 *iat = (IMAGE_THUNK_DATA32 *)(base + d->FirstThunk);
        for (int i = 0; names[i].u1.AddressOfData; i++) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32)
                continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names[i].u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) == 0)
                return (void **)&iat[i].u1.Function;
        }
    }
    return NULL;
}

static bool install_readfile_hook(unsigned char *base)
{
    void **slot = find_iat_slot(base, "KERNEL32.dll", "ReadFile");
    if (!slot) {
        LOG(0, "profiler: ReadFile import not found, ReadFile counts off");
        return false;
    }
    if ((unsigned char *)slot != rebase(VA_READFILE_IAT))
        LOG(0, "profiler: ReadFile IAT slot @%p, expected @%p (continuing)", slot,
            rebase(VA_READFILE_IAT));
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    void *want = k32 ? (void *)GetProcAddress(k32, "ReadFile") : NULL;
    void *cur = *slot;
    if (!want || cur != want) {
        LOG(0, "profiler: ReadFile IAT @%p holds %p, kernel32!ReadFile is %p: mismatch, SKIPPED",
            slot, cur, want);
        return false;
    }
    g_realReadFile = (ReadFile_fn)cur;
    DWORD old;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) {
        LOG(0, "profiler: ReadFile IAT VirtualProtect failed, err %lu, SKIPPED", GetLastError());
        return false;
    }
    InterlockedExchange((volatile LONG *)slot, (LONG)(uintptr_t)(void *)readfile_detour);
    DWORD tmp;
    VirtualProtect(slot, 4, old, &tmp);
    LOG(0, "profiler: ReadFile IAT @%p patched %p -> %p", slot, cur, (void *)readfile_detour);
    return true;
}

// ---------------------------------------------------------------- modules

// Executable sections of the PE image at base, appended to r.
static int image_code_ranges(const unsigned char *base, CodeRange *r, int room)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    int k = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections && k < room; i++) {
        if (!(s[i].Characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE)))
            continue;
        uint32_t size = s[i].Misc.VirtualSize ? s[i].Misc.VirtualSize : s[i].SizeOfRawData;
        if (!size)
            continue;
        r[k].lo = (uint32_t)(uintptr_t)base + s[i].VirtualAddress;
        r[k].hi = r[k].lo + size;
        k++;
    }
    return k;
}

static int range_cmp(const void *a, const void *b)
{
    uint32_t x = ((const CodeRange *)a)->lo, y = ((const CodeRange *)b)->lo;
    return x < y ? -1 : x > y;
}

static int mod_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Mod *)a)->base, y = ((const Mod *)b)->base;
    return x < y ? -1 : x > y;
}

// Sampler thread, nothing suspended.
static void refresh_modules(void)
{
    HMODULE list[MAX_MODS];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), list, sizeof(list), &need))
        return;
    int n = (int)(need / sizeof(HMODULE));
    if (n > MAX_MODS)
        n = MAX_MODS;
    // Unchanged set: nothing to do.
    if (n == g_nmods) {
        bool same = true;
        for (int i = 0; i < n && same; i++) {
            bool found = false;
            for (int j = 0; j < g_nmods; j++)
                if (g_mods[j].base == (uint32_t)(uintptr_t)list[i]) { found = true; break; }
            same = found;
        }
        if (same)
            return;
    }
    static Mod nm[MAX_MODS];
    int k = 0;
    for (int i = 0; i < n; i++) {
        HMODULE ref = NULL;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)list[i], &ref) ||
            ref != list[i]) {
            if (ref) FreeLibrary(ref);
            continue;
        }
        MODULEINFO mi;
        if (!K32GetModuleInformation(GetCurrentProcess(), list[i], &mi, sizeof(mi))) {
            FreeLibrary(ref);
            continue;
        }
        Mod &m = nm[k++];
        m.base = (uint32_t)(uintptr_t)list[i];
        m.size = mi.SizeOfImage;
        m.ref = ref;
        if (!K32GetModuleBaseNameA(GetCurrentProcess(), list[i], m.name, sizeof(m.name)))
            _snprintf(m.name, sizeof(m.name), "mod_%08x", m.base);
        m.name[sizeof(m.name) - 1] = 0;
    }
    qsort(nm, k, sizeof(Mod), mod_cmp);
    int nr = 0;
    for (int i = 0; i < k && nr < MAX_RANGES; i++)
        nr += image_code_ranges((const unsigned char *)(uintptr_t)nm[i].base, g_ranges + nr,
                                MAX_RANGES - nr);
    qsort(g_ranges, nr, sizeof(CodeRange), range_cmp);
    EnterCriticalSection(&g_modCs);
    static Mod old[MAX_MODS];
    int nold = g_nmods;
    memcpy(old, g_mods, sizeof(Mod) * nold);
    memcpy(g_mods, nm, sizeof(Mod) * k);
    g_nmods = k;
    g_nranges = nr;
    LeaveCriticalSection(&g_modCs);
    for (int i = 0; i < nold; i++)
        FreeLibrary(old[i].ref);
}

// Under g_modCs.
static const Mod *find_mod(uint32_t a)
{
    int lo = 0, hi = g_nmods - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (a < g_mods[mid].base)
            hi = mid - 1;
        else if (a >= g_mods[mid].base + g_mods[mid].size)
            lo = mid + 1;
        else
            return &g_mods[mid];
    }
    return NULL;
}

// Nearest named export at or below rva (within 4 KiB). Under g_modCs.
static const char *nearest_export(const Mod *m, uint32_t rva, uint32_t *exprva)
{
    const unsigned char *base = (const unsigned char *)(uintptr_t)m->base;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return NULL;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return NULL;
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress || !dd.Size || dd.VirtualAddress >= m->size)
        return NULL;
    const IMAGE_EXPORT_DIRECTORY *ed = (const IMAGE_EXPORT_DIRECTORY *)(base + dd.VirtualAddress);
    const uint32_t *funcs = (const uint32_t *)(base + ed->AddressOfFunctions);
    const uint32_t *names = (const uint32_t *)(base + ed->AddressOfNames);
    const uint16_t *ords = (const uint16_t *)(base + ed->AddressOfNameOrdinals);
    const char *best = NULL;
    uint32_t bestRva = 0;
    for (uint32_t i = 0; i < ed->NumberOfNames; i++) {
        uint16_t o = ords[i];
        if (o >= ed->NumberOfFunctions)
            continue;
        uint32_t f = funcs[o];
        if (f >= dd.VirtualAddress && f < dd.VirtualAddress + dd.Size)
            continue;   // forwarder
        if (f <= rva && (!best || f > bestRva)) {
            best = (const char *)(base + names[i]);
            bestRva = f;
        }
    }
    if (!best || rva - bestRva > 0x1000)
        return NULL;   // farther than that is usually an unexported function
    *exprva = bestRva;
    return best;
}

// Symbolize a runtime address. key receives the function start (exe symbol
// or module export) or the address itself. Under g_modCs.
static void symbolize_nolock(uint32_t a, char *out, size_t n, uint32_t *key)
{
    uint32_t base = (uint32_t)(uintptr_t)g_base;
    if (key) *key = a;
    if (a >= base && a - base < g_imageSize) {
        uint32_t va = a - base + VA_BASE, off = 0;
        const Sym *s = g_syms.n ? symtab_find(&g_syms, va, &off) : NULL;
        if (s && (s->size == 0 || off < s->size)) {
            if (key) *key = s->va - VA_BASE + base;
            if (off)
                _snprintf(out, n, "%s+0x%x", s->name, off);
            else
                _snprintf(out, n, "%s", s->name);
        } else if (s && off < 0x10000) {
            _snprintf(out, n, "?%s+0x%x", s->name, off);
        } else {
            _snprintf(out, n, "DXHRDC+0x%x", va - VA_BASE);
        }
        out[n - 1] = 0;
        return;
    }
    const Mod *m = find_mod(a);
    if (m) {
        uint32_t rva = a - m->base, er = 0;
        const char *e = nearest_export(m, rva, &er);
        if (e) {
            if (key) *key = m->base + er;
            _snprintf(out, n, "%s!%s+0x%x", m->name, e, rva - er);
        } else {
            _snprintf(out, n, "%s+0x%x", m->name, rva);
        }
    } else {
        _snprintf(out, n, "?");
    }
    out[n - 1] = 0;
}

// Takes g_modCs per call, so the sampler's module refresh is never held up
// for a whole dump.
static void symbolize(uint32_t a, char *out, size_t n, uint32_t *key)
{
    EnterCriticalSection(&g_modCs);
    symbolize_nolock(a, out, n, key);
    LeaveCriticalSection(&g_modCs);
}

// ---------------------------------------------------------------- threads

static uint32_t thread_start_va(DWORD tid)
{
    if (!g_ntqit)
        return 0;
    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h)
        return 0;
    void *start = NULL;
    LONG st = g_ntqit(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), NULL);
    CloseHandle(h);
    if (st < 0 || !start)
        return 0;
    g_startAddrWorks = true;
    uint32_t a = (uint32_t)(uintptr_t)start, base = (uint32_t)(uintptr_t)g_base;
    if (a >= base && a - base < g_imageSize)
        return a - base + VA_BASE;
    return 0;
}

static int role_of_start(uint32_t va)
{
    if (va == VA_RENDER_THREAD) return ROLE_RENDER;
    if (va == VA_ASYNC_WORKER) return ROLE_ASYNC;
    if (va == VA_FS_THREAD) return ROLE_FS;
    if (g_po.extra_threads)
        for (size_t i = 0; i < sizeof(kExtraStarts) / sizeof(kExtraStarts[0]); i++)
            if (va == kExtraStarts[i]) return ROLE_EXTRA;
    return ROLE_NONE;
}

static bool have_slot(DWORD tid)
{
    for (LONG i = 0; i < g_nslots; i++)
        if (g_slots[i].tid == tid && g_slots[i].active)
            return true;
    return false;
}

static bool role_taken(int role)
{
    if (role == ROLE_EXTRA)
        return false;
    for (LONG i = 0; i < g_nslots; i++)
        if (g_slots[i].role == role && g_slots[i].active)
            return true;
    return false;
}

static void add_slot(DWORD tid, int role, uint32_t startVa)
{
    if (g_nslots >= MAX_SLOTS)
        return;
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, tid);
    if (!h) {
        LOG(1, "profiler: OpenThread %lu (%s) failed, err %lu", tid, kRoleName[role],
            GetLastError());
        return;
    }
    struct { LONG status; void *teb; void *pid, *tid; ULONG_PTR aff; LONG prio, base; } tbi;
    memset(&tbi, 0, sizeof(tbi));
    if (!g_ntqit || g_ntqit(h, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), NULL) < 0 ||
        !tbi.teb) {
        LOG(1, "profiler: no TEB for thread %lu (%s), not sampled", tid, kRoleName[role]);
        CloseHandle(h);
        return;
    }
    Slot &s = g_slots[g_nslots];
    s.tid = tid;
    s.h = h;
    s.role = role;
    s.startVa = startVa;
    s.teb = (const uint8_t *)tbi.teb;
    s.samples = s.fails = s.noStack = 0;
    s.active = 1;
    BARRIER();
    LONG idx = g_nslots;
    g_nslots = idx + 1;
    char sym[160] = "";
    if (startVa)
        symbolize(startVa - VA_BASE + (uint32_t)(uintptr_t)g_base, sym, sizeof(sym), NULL);
    LOG(1, "profiler: tracking thread %lu as %s (slot %ld, start 0x%06lX %s)", tid,
        kRoleName[role], (long)idx, (unsigned long)startVa, sym);
}

// Sampler thread, nothing suspended.
static void refresh_threads(void)
{
    // Drop exited threads.
    for (LONG i = 0; i < g_nslots; i++) {
        Slot &s = g_slots[i];
        if (s.active && WaitForSingleObject(s.h, 0) == WAIT_OBJECT_0) {
            s.active = 0;
            LOG(1, "profiler: thread %lu (%s) exited", s.tid, kRoleName[s.role]);
        }
    }
    if (g_mainTid && !have_slot(g_mainTid) && !role_taken(ROLE_MAIN))
        add_slot(g_mainTid, ROLE_MAIN, 0);
    DWORD rt = (DWORD)g_renderTid;
    if (rt && !have_slot(rt) && !role_taken(ROLE_RENDER))
        add_slot(rt, ROLE_RENDER, thread_start_va(rt));

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid)
            continue;
        DWORD tid = te.th32ThreadID;
        if (tid == g_samplerTid || tid == g_writerTid || tid == g_mainTid || have_slot(tid))
            continue;
        bool ignored = false;
        for (int i = 0; i < g_nignore; i++)
            if (g_ignore[i] == tid) { ignored = true; break; }
        if (ignored)
            continue;
        uint32_t sv = thread_start_va(tid);
        int role = role_of_start(sv);
        if (role == ROLE_RENDER && rt && rt != tid)
            role = ROLE_NONE;
        if (role != ROLE_NONE && !role_taken(role))
            add_slot(tid, role, sv);
        else if (g_nignore < IGNORE_N)
            g_ignore[g_nignore++] = tid;
    }
    CloseHandle(snap);

    // Fallback for the async worker when start addresses are unavailable.
    if (!g_startAddrWorks && !role_taken(ROLE_ASYNC)) {
        unsigned char *dm = (unsigned char *)*g_dmPtr;
        if (dm) {
            HANDLE wh = *(HANDLE volatile *)(dm + DM_WORKER_HANDLE);
            DWORD wt = wh ? GetThreadId(wh) : 0;
            if (wt && !have_slot(wt))
                add_slot(wt, ROLE_ASYNC, VA_ASYNC_WORKER);
        }
    }
}

// ---------------------------------------------------------------- sampling

// The suspended window: SuspendThread .. ResumeThread. Only kernel32 thread
// calls and plain memory reads in between.
static void sample_slot(LONG i)
{
    Slot &s = g_slots[i];
    int64_t ts = qpc();
    if (SuspendThread(s.h) == (DWORD)-1) {
        s.fails++;
        return;
    }
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    BOOL ok = GetThreadContext(s.h, &ctx);
    Sample *smp = &g_ring[g_ringHead % g_ringCap];
    if (ok) {
        smp->t = qpc();
        smp->slot = (uint16_t)i;
        smp->eip = ctx.Eip;
        smp->n = 0;
        uint32_t esp = ctx.Esp;
        uint32_t stackBase = *(const volatile uint32_t *)(s.teb + 4);
        uint32_t stackLimit = *(const volatile uint32_t *)(s.teb + 8);
        if (esp >= stackLimit && esp < stackBase && (esp & 3) == 0) {
            uint32_t bytes = stackBase - esp;
            if (bytes > STACK_SCAN_BYTES)
                bytes = STACK_SCAN_BYTES;
            smp->n = (uint16_t)prof_scan_stack((const uint32_t *)(uintptr_t)esp, bytes / 4,
                                               g_ranges, g_nranges, smp->ra, MAX_RA);
        } else {
            s.noStack++;
        }
    }
    ResumeThread(s.h);
    int64_t d = qpc() - ts;
    s.suspTicks += d;
    if (d > s.suspMax)
        s.suspMax = d;
    if (ok) {
        BARRIER();
        g_ringHead = g_ringHead + 1;
        s.samples++;
    } else {
        s.fails++;
    }
}

// Copy the samples of [t0, t1] into g_snap. Sampler thread.
static void snapshot_window(const HitchReq &r)
{
    uint32_t head = g_ringHead;
    uint32_t avail = head < g_ringCap ? head : g_ringCap;
    uint32_t n = 0;
    bool reachedOlder = false;
    for (uint32_t k = 1; k <= avail; k++) {
        const Sample &s = g_ring[(head - k) % g_ringCap];
        if (s.t > r.t1)
            continue;
        if (s.t < r.t0) {
            reachedOlder = true;
            break;
        }
        g_snap[n++] = s;
    }
    g_snapN = n;
    g_snapTruncated = !reachedOlder && head > g_ringCap;
    g_snapReq = r;
}

static void log_status(void)
{
    char thr[900];
    int k = 0;
    thr[0] = 0;
    for (LONG i = 0; i < g_nslots && k < (int)sizeof(thr) - 64; i++) {
        const Slot &s = g_slots[i];
        LONG tot = s.samples + s.fails;
        int m = _snprintf(thr + k, sizeof(thr) - k - 1,
                          "%s%s %lu%s: %ld ok %ld fail, suspended avg %ld us max %ld us", k ? ", " : "",
                          kRoleName[s.role], s.tid, s.active ? "" : " (exited)", (long)s.samples,
                          (long)s.fails, (long)(tot ? ticks_to_us(s.suspTicks / tot) : 0),
                          (long)ticks_to_us(s.suspMax));
        if (m < 0) break;
        k += m;
    }
    thr[sizeof(thr) - 1] = 0;
    LONG ticks = g_ticks;
    LOG(1, "profiler: ticks %ld, avg tick %ld us, max tick %ld us, frames %ld, max frame %ld.%03ld ms, "
           "hitches %ld, dumps queued %ld written %ld dropped %ld, readfile %ld; %s",
        (long)ticks, (long)(ticks ? ticks_to_us(g_tickTotal / ticks) : 0),
        (long)ticks_to_us(g_tickMax), (long)g_frames, (long)(g_maxFrameUs / 1000),
        (long)(g_maxFrameUs % 1000), (long)g_hitches, (long)g_dumpsQueued, (long)g_dumpsWritten,
        (long)g_reqDropped, (long)g_rfCalls, thr);
}

typedef UINT (WINAPI *timePeriod_fn)(UINT);

static DWORD WINAPI sampler_thread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HMODULE winmm = LoadLibraryW(L"winmm.dll");
    timePeriod_fn tbp = winmm ? (timePeriod_fn)(void *)GetProcAddress(winmm, "timeBeginPeriod") : NULL;
    if (tbp)
        tbp(1);
    refresh_modules();
    refresh_threads();
    LOG(1, "profiler: sampler running at %d Hz, ring %u samples (%d ms), %d code ranges in %d modules, "
           "start addresses %s",
        g_po.hz, g_ringCap, g_po.ring_ms, g_nranges, g_nmods,
        g_startAddrWorks ? "available" : "UNAVAILABLE (fallbacks only)");
    g_samplerRunning = 1;
    int64_t next = qpc();
    int64_t nextRefresh = next + g_qpcFreq * 2;
    int64_t nextStatus = next + g_qpcFreq * 10;
    int64_t nextIgnoreReset = next + g_qpcFreq * 30;
    bool maxLogged = false;
    for (;;) {
        int64_t t0 = qpc();
        if (t0 >= nextRefresh) {
            if (t0 >= nextIgnoreReset) {
                g_nignore = 0;
                nextIgnoreReset = t0 + g_qpcFreq * 30;
            }
            refresh_modules();
            refresh_threads();
            nextRefresh = qpc() + g_qpcFreq * 2;
        }
        int64_t ts = qpc();
        LONG ns = g_nslots;
        for (LONG i = 0; i < ns; i++)
            if (g_slots[i].active)
                sample_slot(i);
        int64_t te = qpc();
        g_tickTotal += te - ts;
        if (te - ts > g_tickMax)
            g_tickMax = te - ts;
        InterlockedIncrement(&g_ticks);

        // Hand a finished hitch window to the writer.
        while (g_reqTail != g_reqHead && !g_writerBusy) {
            const HitchReq &r = g_req[g_reqTail & (REQ_N - 1)];
            if (r.t1 >= ts)
                break;   // wait until a whole tick ran after the window end
            snapshot_window(r);
            BARRIER();
            g_reqTail = g_reqTail + 1;
            g_writerBusy = 1;
            SetEvent(g_writerEvent);
        }
        if (!maxLogged && g_dumpsQueued >= g_po.max_dumps) {
            maxLogged = true;
            LOG(1, "profiler: profiler_max_dumps (%d) reached, no more dumps", g_po.max_dumps);
        }
        if (te >= nextStatus) {
            log_status();
            nextStatus = te + g_qpcFreq * 10;
        }

        next += g_period;
        int64_t now = qpc();
        if (now - next > g_period * 10)
            next = now;   // fell far behind: do not burst
        int64_t remain = next - now;
        if (remain > 0) {
            DWORD ms = (DWORD)(remain * 1000 / g_qpcFreq);
            Sleep(ms ? ms : 1);
        }
    }
    return 0;
}

// ---------------------------------------------------------------- writer

struct Buf {
    char *p;
    size_t n, cap;
};

static void bput(Buf *b, const char *fmt, ...)
{
    if (!b->p || b->n + 2 >= b->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnprintf(b->p + b->n, b->cap - b->n - 1, fmt, ap);
    va_end(ap);
    if (m < 0)
        m = (int)(b->cap - b->n - 1);
    b->n += m;
    b->p[b->n] = 0;
}

static void bwrite(Buf *b)
{
    if (g_hitchLog != INVALID_HANDLE_VALUE && b->n) {
        DWORD w;
        WriteFile(g_hitchLog, b->p, (DWORD)b->n, &w, NULL);
    }
    b->n = 0;
}

static void wall_time(int64_t t, char *out, size_t n)
{
    uint64_t ft = g_wall0;
    int64_t d = t - g_qpc0;
    if (d < 0)
        d = 0;
    ft += (uint64_t)(d / g_qpcFreq) * 10000000ull + (uint64_t)(d % g_qpcFreq) * 10000000ull / g_qpcFreq;
    FILETIME f, lf;
    f.dwLowDateTime = (DWORD)ft;
    f.dwHighDateTime = (DWORD)(ft >> 32);
    SYSTEMTIME st;
    if (FileTimeToLocalFileTime(&f, &lf) && FileTimeToSystemTime(&lf, &st))
        _snprintf(out, n, "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    else
        _snprintf(out, n, "?");
    out[n - 1] = 0;
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

struct KC {
    uint32_t k, c;
};

static int kc_desc(const void *a, const void *b)
{
    const KC *x = (const KC *)a, *y = (const KC *)b;
    if (x->c != y->c) return x->c > y->c ? -1 : 1;
    return x->k < y->k ? -1 : x->k > y->k;
}

// arr sorted; out receives {value, run length}, sorted by count desc.
static int count_runs(const uint32_t *arr, int n, KC *out)
{
    int m = 0;
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && arr[j] == arr[i])
            j++;
        out[m].k = arr[i];
        out[m].c = (uint32_t)(j - i);
        m++;
        i = j;
    }
    qsort(out, m, sizeof(KC), kc_desc);
    return m;
}

// Function key of an address (symbol start), with a small sorted cache.
struct KeyCache {
    uint32_t *addr, *key;
    int n;
};

static void keycache_build(KeyCache *kc, uint32_t *addrs, int n)
{
    qsort(addrs, n, sizeof(uint32_t), u32_cmp);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (!m || addrs[m - 1] != addrs[i])
            addrs[m++] = addrs[i];
    kc->addr = addrs;
    kc->n = m;
    kc->key = (uint32_t *)malloc(sizeof(uint32_t) * (m ? m : 1));
    char tmp[200];
    for (int i = 0; i < m; i++)
        symbolize(addrs[i], tmp, sizeof(tmp), &kc->key[i]);
}

static uint32_t keycache_get(const KeyCache *kc, uint32_t a)
{
    int lo = 0, hi = kc->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (kc->addr[mid] == a) return kc->key[mid];
        if (kc->addr[mid] < a) lo = mid + 1; else hi = mid - 1;
    }
    return a;
}

struct SigEnt {
    uint32_t hash;
    uint32_t idx;
};

static int sig_cmp(const void *a, const void *b)
{
    const SigEnt *x = (const SigEnt *)a, *y = (const SigEnt *)b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

static void dump_thread(Buf *b, LONG slot, const Sample *smp, uint32_t n, bool stacks)
{
    const Slot &s = g_slots[slot];
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n; i++)
        cnt += smp[i].slot == slot;
    bput(b, "\n-- thread %s (tid %lu): %u samples\n", kRoleName[s.role], s.tid, cnt);
    if (!cnt)
        return;
    char sym[200];

    // Leaf histogram by function.
    uint32_t *eips = (uint32_t *)malloc(sizeof(uint32_t) * cnt);
    uint32_t *keys = (uint32_t *)malloc(sizeof(uint32_t) * cnt);
    KC *kc = (KC *)malloc(sizeof(KC) * cnt);
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++)
        if (smp[i].slot == slot)
            eips[m++] = smp[i].eip;
    KeyCache cache;
    keycache_build(&cache, eips, (int)m);   // eips is now the unique sorted set
    m = 0;
    for (uint32_t i = 0; i < n; i++)
        if (smp[i].slot == slot)
            keys[m++] = keycache_get(&cache, smp[i].eip);
    qsort(keys, m, sizeof(uint32_t), u32_cmp);
    int nk = count_runs(keys, (int)m, kc);
    bput(b, "  leaf functions (samples, pct, address, symbol):\n");
    for (int i = 0; i < nk && i < 20; i++) {
        symbolize(kc[i].k, sym, sizeof(sym), NULL);
        bput(b, "    %5u %5.1f%%  %08x  %s\n", kc[i].c, 100.0 * kc[i].c / cnt, kc[i].k, sym);
    }

    // Return address histogram, each address counted once per sample.
    uint32_t total = 0;
    for (uint32_t i = 0; i < n; i++)
        if (smp[i].slot == slot)
            total += smp[i].n;
    uint32_t *ras = (uint32_t *)malloc(sizeof(uint32_t) * (total ? total : 1));
    KC *rc = (KC *)malloc(sizeof(KC) * (total ? total : 1));
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (smp[i].slot != slot)
            continue;
        uint32_t tmp[MAX_RA];
        memcpy(tmp, smp[i].ra, smp[i].n * sizeof(uint32_t));
        qsort(tmp, smp[i].n, sizeof(uint32_t), u32_cmp);
        for (int j = 0; j < smp[i].n; j++)
            if (!j || tmp[j] != tmp[j - 1])
                ras[r++] = tmp[j];
    }
    qsort(ras, r, sizeof(uint32_t), u32_cmp);
    int nr = count_runs(ras, (int)r, rc);
    bput(b, "  return addresses on stack (samples, pct, address, call site):\n");
    for (int i = 0; i < nr && i < 30; i++) {
        symbolize(rc[i].k, sym, sizeof(sym), NULL);
        bput(b, "    %5u %5.1f%%  %08x  %s\n", rc[i].c, 100.0 * rc[i].c / cnt, rc[i].k, sym);
    }

    if (stacks) {
        // Top 5 stacks by signature (leaf function + return addresses).
        SigEnt *se = (SigEnt *)malloc(sizeof(SigEnt) * cnt);
        uint32_t q = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (smp[i].slot != slot)
                continue;
            uint32_t lk = keycache_get(&cache, smp[i].eip);
            uint32_t h = prof_hash(&lk, 1, 2166136261u);
            se[q].hash = prof_hash(smp[i].ra, smp[i].n, h);
            se[q].idx = i;
            q++;
        }
        qsort(se, q, sizeof(SigEnt), sig_cmp);
        KC *grp = (KC *)malloc(sizeof(KC) * q);
        int ng = 0;
        for (uint32_t i = 0; i < q;) {
            uint32_t j = i;
            while (j < q && se[j].hash == se[i].hash)
                j++;
            grp[ng].k = se[i].idx;     // representative sample
            grp[ng].c = j - i;
            ng++;
            i = j;
        }
        qsort(grp, ng, sizeof(KC), kc_desc);
        bput(b, "  top stacks (leaf first, then return addresses):\n");
        for (int g = 0; g < ng && g < 5; g++) {
            const Sample &x = smp[grp[g].k];
            symbolize(x.eip, sym, sizeof(sym), NULL);
            bput(b, "    #%d: %u samples (%.1f%%)\n      %08x  %s\n", g + 1, grp[g].c,
                 100.0 * grp[g].c / cnt, x.eip, sym);
            for (int j = 0; j < x.n; j++) {
                symbolize(x.ra[j], sym, sizeof(sym), NULL);
                bput(b, "      %08x  %s\n", x.ra[j], sym);
            }
        }
        free(grp);
        free(se);
    }
    free(cache.key);
    free(ras);
    free(rc);
    free(kc);
    free(keys);
    free(eips);
}

struct RfAgg {
    DWORD tid;
    uint32_t calls, overlapped;
    uint64_t bytes;
    int64_t ticks, maxTicks;
};

static void dump_readfile(Buf *b, const HitchReq &r)
{
    if (!g_realReadFile) {
        bput(b, "\n-- ReadFile: not hooked\n");
        return;
    }
    RfAgg agg[32];
    int na = 0;
    uint32_t head = (uint32_t)g_rfHead;
    uint32_t avail = head < (uint32_t)RF_N ? head : (uint32_t)RF_N;
    bool older = false;
    uint32_t other = 0;
    for (uint32_t k = 1; k <= avail; k++) {
        uint32_t idx = head - k;
        const RfEvent &e = g_rf[idx & (RF_N - 1)];
        uint32_t s1 = e.seq;
        BARRIER();
        RfEvent c;
        c.tid = e.tid;
        c.req = e.req;
        c.got = e.got;
        c.t0 = e.t0;
        c.t1 = e.t1;
        BARRIER();
        if (s1 != idx + 1 || e.seq != s1)
            continue;   // being written or overwritten
        if (c.t1 < r.t0) {
            older = true;
            break;
        }
        if (c.t0 > r.t1)
            continue;
        int a = 0;
        while (a < na && agg[a].tid != c.tid)
            a++;
        if (a == na) {
            if (na == 32) { other++; continue; }
            memset(&agg[na], 0, sizeof(RfAgg));
            agg[na].tid = c.tid;
            na++;
        }
        agg[a].calls++;
        if (c.req & 0x80000000u)
            agg[a].overlapped++;
        agg[a].bytes += (c.req & 0x80000000u) ? (c.req & 0x7FFFFFFF) : c.got;
        int64_t d = c.t1 - c.t0;
        agg[a].ticks += d;
        if (d > agg[a].maxTicks)
            agg[a].maxTicks = d;
    }
    bput(b, "\n-- ReadFile calls overlapping the window%s:\n",
         (!older && head > (uint32_t)RF_N) ? " (event ring wrapped, partial)" : "");
    if (!na)
        bput(b, "    none\n");
    for (int a = 0; a < na; a++) {
        const char *who = "other";
        for (LONG i = 0; i < g_nslots; i++)
            if (g_slots[i].tid == agg[a].tid) { who = kRoleName[g_slots[i].role]; break; }
        LONG us = ticks_to_us(agg[a].ticks), mx = ticks_to_us(agg[a].maxTicks);
        bput(b, "    tid %lu (%s): %u calls (%u overlapped), %llu bytes, %ld.%03ld ms inside ReadFile, max %ld.%03ld ms\n",
             agg[a].tid, who, agg[a].calls, agg[a].overlapped, (unsigned long long)agg[a].bytes,
             (long)(us / 1000), (long)(us % 1000), (long)(mx / 1000), (long)(mx % 1000));
    }
    if (other)
        bput(b, "    (%u calls from more threads not listed)\n", other);
}

static void write_dump(Buf *b)
{
    const HitchReq r = g_snapReq;
    const uint32_t n = g_snapN;
    LONG id = InterlockedIncrement(&g_dumpsWritten);
    char w0[32], w1[32];
    wall_time(r.t0, w0, sizeof(w0));
    wall_time(r.t1, w1, sizeof(w1));
    LONG us = ticks_to_us(r.t1 - r.t0);
    bput(b, "\n==================== hitch #%ld: frame %ld.%03ld ms (frame #%ld), %s .. %s, %u samples%s\n",
         (long)id, (long)(us / 1000), (long)(us % 1000), (long)r.frame, w0, w1, n,
         g_snapTruncated ? " (window longer than the ring: oldest part missing)" : "");
    char line[1024];
    if (dxhrfix_hooks_format_totals(line, sizeof(line)))
        bput(b, "%s\n", line);
    else
        bput(b, "totals: hooks not installed\n");
    if (dxhrfix_hooks_format_precreate(line, sizeof(line)))
        bput(b, "%s\n", line);
    bput(b, "profiler: frames %ld, hitches %ld, readfile calls %ld\n", (long)g_frames,
         (long)g_hitches, (long)g_rfCalls);
    LONG ns = g_nslots;
    // Render and main first, with stacks.
    static const int order[] = {ROLE_RENDER, ROLE_MAIN, ROLE_ASYNC, ROLE_FS, ROLE_EXTRA};
    for (int o = 0; o < 5; o++)
        for (LONG i = 0; i < ns; i++)
            if (g_slots[i].role == order[o])
                dump_thread(b, i, g_snap, n, order[o] == ROLE_RENDER || order[o] == ROLE_MAIN);
    dump_readfile(b, r);
    bwrite(b);
    LOG(1, "profiler: hitch #%ld frame %ld.%03ld ms (frame #%ld) dumped to dxhrfix_hitch.log, %u samples",
        (long)id, (long)(us / 1000), (long)(us % 1000), (long)r.frame, n);
}

static DWORD WINAPI writer_thread(LPVOID)
{
    Buf b;
    b.cap = 1 << 20;
    b.p = (char *)malloc(b.cap);
    b.n = 0;
    for (;;) {
        WaitForSingleObject(g_writerEvent, INFINITE);
        if (g_writerBusy) {
            write_dump(&b);
            BARRIER();
            g_writerBusy = 0;
        }
    }
    return 0;
}

// ---------------------------------------------------------------- install / start

static void load_symbols(void)
{
    wchar_t path[MAX_PATH + 32];
    DWORD e = GetEnvironmentVariableW(L"DXHRFIX_SYMS", path, MAX_PATH);
    if (!e || e >= MAX_PATH) {
        _snwprintf(path, MAX_PATH + 31, L"%ls\\dxhrfix_syms.tsv", g_dllDir);
        path[MAX_PATH + 31] = 0;
    }
    char p8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof(p8), NULL, NULL);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        LOG(0, "profiler: no symbol file %s (err %lu): exe addresses print as DXHRDC+RVA", p8,
            GetLastError());
        return;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (64 << 20)) {
        CloseHandle(f);
        LOG(0, "profiler: symbol file %s has a bad size", p8);
        return;
    }
    char *buf = (char *)malloc((size_t)sz.QuadPart + 1);
    DWORD got = 0;
    if (!buf || !ReadFile(f, buf, (DWORD)sz.QuadPart, &got, NULL)) {
        CloseHandle(f);
        free(buf);
        LOG(0, "profiler: reading %s failed", p8);
        return;
    }
    CloseHandle(f);
    buf[got] = 0;
    g_symBuf = buf;
    int n = symtab_parse(&g_syms, buf, got);
    LOG(0, "profiler: %d symbols loaded from %s", n, p8);
}

int dxhrfix_profiler_install(unsigned char *base, const DxhrfixProfilerOptions *opt,
                             dxhrfix_log_fn log)
{
    g_base = base;
    g_log = log;
    g_logLevel = opt->log_level;
    g_dmPtr = (void *volatile *)rebase(VA_DEVICE_MANAGER);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = f.QuadPart;
    g_po = *opt;
    if (g_po.hz < 50) g_po.hz = 50;
    if (g_po.hz > 4000) g_po.hz = 4000;
    if (g_po.hitch_ms < 1) g_po.hitch_ms = 1;
    if (g_po.max_dumps < 0) g_po.max_dumps = 0;
    if (g_po.ring_ms < 100) g_po.ring_ms = 100;
    if (g_po.ring_ms > 4000) g_po.ring_ms = 4000;
    wcsncpy(g_outDir, opt->out_dir ? opt->out_dir : L".", MAX_PATH - 1);
    wcsncpy(g_dllDir, opt->dll_dir ? opt->dll_dir : L".", MAX_PATH - 1);
    g_po.out_dir = g_outDir;
    g_po.dll_dir = g_dllDir;
    g_mainTid = GetCurrentThreadId();
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    g_imageSize = nt->OptionalHeader.SizeOfImage;
    g_hitchTicks = g_qpcFreq * g_po.hitch_ms / 1000;

    if (!install_one("present", VA_PRESENT, kPresentOrig, sizeof(kPresentOrig), kPresentAbs, 2, 9,
                     (void *)present_detour, (void **)&g_presentTramp)) {
        LOG(0, "profiler: Present hook not installed, profiler OFF");
        return 0;
    }
    int applied = 1;
    if (g_po.readfile)
        applied += install_readfile_hook(base);
    g_profInstalled = true;
    LOG(0, "profiler: installed (%d patches), main thread %lu, hz %d, hitch_ms %d, max_dumps %d, "
           "ring_ms %d, extra_threads %d, readfile %d",
        applied, g_mainTid, g_po.hz, g_po.hitch_ms, g_po.max_dumps, g_po.ring_ms,
        g_po.extra_threads, g_po.readfile);
    return applied;
}

void dxhrfix_profiler_start(void)
{
    static volatile LONG started;
    if (!g_profInstalled || InterlockedExchange(&started, 1))
        return;
    InitializeCriticalSection(&g_modCs);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    g_ntqit = ntdll ? (NtQIT_fn)(void *)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
    load_symbols();

    g_ringCap = (uint32_t)((int64_t)g_po.hz * g_po.ring_ms / 1000) * MAX_SLOTS;
    if (g_ringCap < 1024) g_ringCap = 1024;
    size_t bytes = (size_t)g_ringCap * sizeof(Sample);
    g_ring = (Sample *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_snap = (Sample *)VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_writerEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_ring || !g_snap || !g_writerEvent) {
        LOG(0, "profiler: allocation failed (%lu bytes x2), profiler OFF", (unsigned long)bytes);
        return;
    }

    wchar_t path[MAX_PATH + 32];
    _snwprintf(path, MAX_PATH + 31, L"%ls\\dxhrfix_hitch.log", g_outDir);
    path[MAX_PATH + 31] = 0;
    g_hitchLog = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    char p8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof(p8), NULL, NULL);
    if (g_hitchLog == INVALID_HANDLE_VALUE) {
        LOG(0, "profiler: cannot create %s (err %lu), profiler OFF", p8, GetLastError());
        return;
    }
    g_qpc0 = qpc();
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    g_wall0 = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    {
        Buf b;
        char hdr[1024];
        b.p = hdr;
        b.cap = sizeof(hdr);
        b.n = 0;
        char w[32];
        wall_time(g_qpc0, w, sizeof(w));
        bput(&b, "dxhrfix hitch profiler, started %s: hz %d, hitch_ms %d, max_dumps %d, ring_ms %d, "
                 "extra_threads %d, readfile %d, symbols %d, exe base %p\r\n"
                 "Addresses are runtime addresses (exe base 0x400000 means they equal the VAs in DXHRDC.objdump.txt).\r\n",
             w, g_po.hz, g_po.hitch_ms, g_po.max_dumps, g_po.ring_ms, g_po.extra_threads,
             g_po.readfile, g_syms.n, g_base);
        bwrite(&b);
    }

    g_period = g_qpcFreq / g_po.hz;
    if (g_period < 1) g_period = 1;
    HANDLE wt = CreateThread(NULL, 0x40000, writer_thread, NULL, 0, &g_writerTid);
    if (!wt) {
        LOG(0, "profiler: writer CreateThread failed, err %lu, profiler OFF", GetLastError());
        return;
    }
    CloseHandle(wt);
    HANDLE st = CreateThread(NULL, 0x40000, sampler_thread, NULL, 0, &g_samplerTid);
    if (!st) {
        LOG(0, "profiler: sampler CreateThread failed, err %lu, profiler OFF", GetLastError());
        return;
    }
    CloseHandle(st);
    LOG(0, "profiler: started, hitch log %s, sampler tid %lu, writer tid %lu", p8, g_samplerTid,
        g_writerTid);
}
