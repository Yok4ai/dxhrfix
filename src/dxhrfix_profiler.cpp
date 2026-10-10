// Sampling hitch profiler for DXHRDC.exe.
//
// Frame boundary: the engine's Present wrapper 0x54C9F0 (thiscall
// ecx=RenderContext, 3 stack args, ret 0xC, called only from 0x53A584 in the
// render frame function 0x53A170) is hooked with a trampoline. The time
// between two successive Present returns on the render thread is the frame
// time. The first thread that calls it is the render thread.
//
// Threads: main = the thread that ran DllMain PROCESS_ATTACH (static import,
// so the process's initial thread). The others come from a Toolhelp thread
// snapshot every 2 s, classified by Win32 start address (NtQueryInformationThread
// class 9) and thread description: render 0x53BAD0, async create worker
// 0x54A100, threaded file system worker 0x4A2E10 (loop 0x49FB40), other exe
// threads (start inside the exe), DXVK threads (start in d3d11.dll / dxgi.dll
// or a "dxvk" description), other threads. profiler_extra_threads: 0 = the
// four core threads, 1 = plus exe threads, 2 = every thread; at most 16 are
// sampled, chosen in that order. Fallbacks: render = the Present caller,
// async worker = GetThreadId of the handle at deviceManager+0xA180.
//
// Native attribution: a thread inside a syscall or unix call (Wine's syscall
// frame at TEB+0x218 holds exactly the Eip/Esp GetThreadContext returned) has
// its kernel stack below that frame scanned for addresses inside native
// executable mappings, read from Z:\proc\self\maps every 2 s; dumps print
// them as file+offset.
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

// 56 | 8b f1 | 8b 0d [deviceManager] | 8b 01 | 8b 50 04 | ff d2 | 80 78 18 00 | a1 [deviceManager]
static const uint8_t kPresentOrig[25] = {
    0x56, 0x8b, 0xf1, 0x8b, 0x0d, 0x40, 0xb9, 0x2a, 0x01, 0x8b, 0x01, 0x8b, 0x50,
    0x04, 0xff, 0xd2, 0x80, 0x78, 0x18, 0x00, 0xa1, 0x40, 0xb9, 0x2a, 0x01};
static const int kPresentAbs[] = {5, 21};

// ---------------------------------------------------------------- limits

enum { MAX_REC = 256, MAX_ACTIVE = 16, MAX_RA = 24, MAX_NRA = 16, MAX_MODS = 256,
       MAX_RANGES = 512, MAX_NATIVE = 2048, REQ_N = 16, RF_N = 16384, STACK_SCAN_BYTES = 4096,
       KSTACK_SCAN_BYTES = 16384, MAX_EXE_MODE2 = 6 };

// Wine (Proton 11, i386 unix side): TEB+0x218 holds the current syscall
// frame (verified in __wine_syscall_dispatcher: mov %fs:0x218,%ecx). While a
// thread is inside a syscall or unix call, GetThreadContext returns the PE
// state saved in that frame (Eip at +0x8, Esp at +0xC) and the native code
// runs on the kernel stack below the frame.
static const uint32_t TEB_SYSCALL_FRAME = 0x218;
static const uint32_t FRAME_EIP = 0x8, FRAME_ESP = 0xC;
// A suspended thread waits inside Wine's SIGUSR1 handler on its alternate
// signal stack, which lives in the TEB block (64 KiB aligned, after the TEB).
// The kernel's ucontext there holds the interrupted native registers; it is
// found by its i386 gregs signature: es = ds = ss = 0x2b, cs = 0x23 and
// esp == esp_at_signal (gregs 2, 3, 18, 15, 7 == 17). EIP is gregs[14].
static const uint32_t SIG_SCAN_BYTES = 0x4000;

enum Role { ROLE_MAIN, ROLE_RENDER, ROLE_ASYNC, ROLE_FS, ROLE_EXE, ROLE_DXVK, ROLE_OTHER };
static const char *const kRoleName[] = {"main", "render", "async_create", "fs_worker",
                                        "exe_thread", "dxvk", "other"};
static const int kRolePrio[] = {0, 0, 0, 0, 1, 2, 3};

// One record per thread seen. Records are referenced by samples (index), so
// a record is reused only 20 s after its thread exited.
struct Slot {
    DWORD tid;
    HANDLE h;                  // own handle while active
    int role, prio;
    uint32_t start;            // runtime Win32 start address, 0 = unknown
    const uint8_t *teb;
    uint32_t kLo, kHi;         // kernel stack region, 0 = native scan off
    uint32_t sLo, sHi;         // signal stack region (Wine TEB block), 0 = unknown
    volatile LONG active;
    bool exited, failed, seen;
    int64_t exitedAt;
    char name[112];
    volatile LONG samples, fails, noStack, inSys, noSig;
    int64_t suspTicks, suspMax;   // SuspendThread call .. ResumeThread return
};

struct Sample {
    int64_t t;
    uint16_t slot;
    uint8_t n;                 // PE return addresses
    uint8_t nn;                // native return addresses (kernel stack)
    uint32_t eip;
    uint8_t sys;               // context came from the Wine syscall frame
    uint8_t pad[3];
    uint32_t neip;             // native EIP from the signal frame, 0 = unknown
    uint32_t ra[MAX_RA];
    uint32_t nra[MAX_NRA];     // innermost first
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

struct NatRange {
    uint32_t lo, hi, off;      // executable mapping and its file offset
    char name[44];
};

typedef LONG (WINAPI *NtQIT_fn)(HANDLE, int, void *, ULONG, ULONG *);
typedef HRESULT (WINAPI *GetThreadDescription_fn)(HANDLE, PWSTR *);
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
static Slot g_slots[MAX_REC];
static volatile LONG g_nslots;          // records in use (published after init)
static int g_active[MAX_ACTIVE];        // sampler thread only
static int g_nactive;
static Sample *g_ring, *g_snap;
static uint32_t g_ringCap;
static volatile uint32_t g_ringHead;   // samples ever written
static CodeRange g_ranges[MAX_RANGES]; // sampler thread only
static int g_nranges;
static CodeRange g_natRanges[MAX_NATIVE]; // sampler thread only
static int g_nnatRanges;
static NtQIT_fn g_ntqit;
static GetThreadDescription_fn g_getDesc;
static bool g_startAddrWorks;
static int64_t g_period;
static volatile LONG g_ticks;
static int64_t g_tickTotal, g_tickMax;
static DWORD g_samplerTid, g_writerTid;
static char *g_mapsBuf;
static const size_t MAPS_BUF = 4 << 20;
static bool g_mapsOk, g_mapsLogged;

// Modules and native mappings (g_modCs)
static CRITICAL_SECTION g_modCs;
static Mod g_mods[MAX_MODS];
static int g_nmods;
static NatRange g_nat[MAX_NATIVE];
static int g_nnat;

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

static bool install_readfile_hook(void)
{
    void *orig = NULL;
    if (!iat_hook("KERNEL32.dll", "ReadFile", VA_READFILE_IAT, (void *)readfile_detour, &orig))
        return false;
    g_realReadFile = (ReadFile_fn)orig;
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

// Native (unix side) executable mappings from Z:\proc\self\maps. Sampler
// thread, nothing suspended. PE images that Wine maps from files are left
// out (they are symbolized as PE modules); everything else with a path and
// an x permission is kept.
static bool pe_like(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot)
        return false;
    static const char *const ext[] = {".dll", ".exe", ".drv", ".sys", ".cpl", ".ocx", ".acm",
                                      ".ax", ".tlb", ".fon", ".ttf", ".nls", ".mui"};
    for (size_t i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
        if (_stricmp(dot, ext[i]) == 0)
            return true;
    return false;
}

static uint64_t parse_hex(const char *&p)
{
    uint64_t v = 0;
    for (;; p++) {
        char c = *p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return v;
        v = (v << 4) | (uint64_t)d;
    }
}

static wchar_t g_mapsPath[64];

// Whole file into buf (NUL terminated), 0 on failure. Wine opens files in the
// wineserver, so Z:\proc\self would be the server: callers use a pid path.
static size_t read_small_file(const wchar_t *path, char *buf, size_t cap)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    size_t len = 0;
    DWORD got = 0;
    while (len < cap - 1 && ReadFile(f, buf + len, (DWORD)(cap - 1 - len), &got, NULL) && got)
        len += got;
    CloseHandle(f);
    buf[len] = 0;
    return len;
}

static bool mem_contains_ci(const char *p, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++)
        if (_strnicmp(p + i, needle, m) == 0)
            return true;
    return false;
}

// The unix pid of this process: a /proc entry whose command line holds the
// exe name and whose mappings include the 32-bit Wine ntdll.so (the Proton
// launcher's command line holds the exe name too, but it is not Wine).
static bool find_maps_path(void)
{
    static bool tried;
    if (tried)
        return false;
    tried = true;
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (!n || n >= sizeof(exe))
        return false;
    const char *bn = strrchr(exe, '\\');
    bn = bn ? bn + 1 : exe;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"Z:\\proc\\*", &fd);
    if (h == INVALID_HANDLE_VALUE) {
        LOG(1, "profiler: cannot list Z:\\proc (err %lu), no native attribution", GetLastError());
        return false;
    }
    char cmd[4096];
    int found = 0, cands = 0;
    do {
        const wchar_t *nm = fd.cFileName;
        bool num = nm[0] != 0;
        for (const wchar_t *c = nm; *c; c++)
            if (*c < L'0' || *c > L'9') { num = false; break; }
        if (!num)
            continue;
        wchar_t path[64];
        _snwprintf(path, 63, L"Z:\\proc\\%ls\\cmdline", nm);
        path[63] = 0;
        size_t len = read_small_file(path, cmd, sizeof(cmd));
        if (!len || !mem_contains_ci(cmd, len, bn))
            continue;
        cands++;
        _snwprintf(path, 63, L"Z:\\proc\\%ls\\maps", nm);
        size_t ml = read_small_file(path, g_mapsBuf, MAPS_BUF);
        if (ml && strstr(g_mapsBuf, "i386-unix/ntdll.so")) {
            wcscpy(g_mapsPath, path);
            found++;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found)
        LOG(1, "profiler: native mappings from %ls (%d /proc entries named %s)", g_mapsPath, cands, bn);
    else
        LOG(1, "profiler: no /proc entry for %s with i386 ntdll.so (%d candidates), no native "
               "attribution", bn, cands);
    return found > 0;
}

static void refresh_native(void)
{
    if (!g_mapsBuf)
        return;
    if (!g_mapsPath[0] && !find_maps_path())
        return;
    size_t len = read_small_file(g_mapsPath, g_mapsBuf, MAPS_BUF);
    if (!len) {
        if (!g_mapsLogged) {
            g_mapsLogged = true;
            LOG(1, "profiler: %ls not readable, no native attribution", g_mapsPath);
        }
        return;
    }
    static NatRange nr[MAX_NATIVE];
    int k = 0;
    const char *p = g_mapsBuf;
    while (*p && k < MAX_NATIVE) {
        const char *line = p;
        const char *eol = strchr(p, '\n');
        p = eol ? eol + 1 : p + strlen(p);
        const char *q = line;
        uint64_t lo = parse_hex(q);
        if (*q != '-') continue;
        q++;
        uint64_t hi = parse_hex(q);
        if (*q != ' ') continue;
        q++;
        bool x = q[2] == 'x';
        while (*q && *q != ' ' && *q != '\n') q++;           // perms
        while (*q == ' ') q++;
        uint64_t off = parse_hex(q);                         // offset
        while (*q == ' ') q++;
        while (*q && *q != ' ' && *q != '\n') q++;           // dev
        while (*q == ' ') q++;
        while (*q && *q != ' ' && *q != '\n') q++;           // inode
        while (*q == ' ') q++;
        if (!x || (*q != '/' && *q != '[') || hi > 0x100000000ull || lo >= hi)
            continue;   // [vdso] holds __kernel_vsyscall, the leaf of every syscall
        const char *pe = eol ? eol : q + strlen(q);
        const char *base = q;
        for (const char *t = q; t < pe; t++)
            if (*t == '/') base = t + 1;
        size_t bl = (size_t)(pe - base);
        if (bl >= sizeof(nr[0].name)) bl = sizeof(nr[0].name) - 1;
        NatRange &r = nr[k];
        memcpy(r.name, base, bl);
        r.name[bl] = 0;
        if (pe_like(r.name))
            continue;
        r.lo = (uint32_t)lo;
        r.hi = (uint32_t)(hi > 0xFFFFFFFFull ? 0xFFFFFFFFull : hi);
        r.off = (uint32_t)off;
        k++;
    }
    // /proc maps are sorted by address already.
    for (int i = 0; i < k; i++) {
        g_natRanges[i].lo = nr[i].lo;
        g_natRanges[i].hi = nr[i].hi;
    }
    g_nnatRanges = k;
    EnterCriticalSection(&g_modCs);
    memcpy(g_nat, nr, sizeof(NatRange) * k);
    g_nnat = k;
    LeaveCriticalSection(&g_modCs);
    if (!g_mapsOk) {
        g_mapsOk = true;
        LOG(1, "profiler: %d native executable mappings from %ls", k, g_mapsPath);
    }
}

// Under g_modCs.
static const NatRange *find_native(uint32_t a)
{
    int lo = 0, hi = g_nnat - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (a < g_nat[mid].lo)
            hi = mid - 1;
        else if (a >= g_nat[mid].hi)
            lo = mid + 1;
        else
            return &g_nat[mid];
    }
    return NULL;
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
        const NatRange *r = find_native(a);
        if (r)
            _snprintf(out, n, "%s+0x%x", r->name, a - r->lo + r->off);
        else
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

static uint32_t thread_start(DWORD tid)
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
    return (uint32_t)(uintptr_t)start;
}

static void thread_desc(DWORD tid, char *out, size_t n)
{
    out[0] = 0;
    if (!g_getDesc)
        return;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!h)
        return;
    PWSTR d = NULL;
    if (SUCCEEDED(g_getDesc(h, &d)) && d) {
        WideCharToMultiByte(CP_UTF8, 0, d, -1, out, (int)n, NULL, NULL);
        out[n - 1] = 0;
        LocalFree(d);
    }
    CloseHandle(h);
}

static uint32_t exe_va(uint32_t a)
{
    uint32_t base = (uint32_t)(uintptr_t)g_base;
    return a >= base && a - base < g_imageSize ? a - base + VA_BASE : 0;
}

static bool start_in_module(uint32_t a, const char *mod)
{
    EnterCriticalSection(&g_modCs);
    const Mod *m = find_mod(a);
    bool r = m && _stricmp(m->name, mod) == 0;
    LeaveCriticalSection(&g_modCs);
    return r;
}

static int classify(DWORD tid, uint32_t start, const char *desc)
{
    if (tid == g_mainTid) return ROLE_MAIN;
    if (g_renderTid && tid == (DWORD)g_renderTid) return ROLE_RENDER;
    uint32_t va = exe_va(start);
    if (va == VA_RENDER_THREAD && !g_renderTid) return ROLE_RENDER;
    if (va == VA_ASYNC_WORKER) return ROLE_ASYNC;
    if (va == VA_FS_THREAD) return ROLE_FS;
    if (va) return ROLE_EXE;
    if (_strnicmp(desc, "dxvk", 4) == 0 || start_in_module(start, "d3d11.dll") ||
        start_in_module(start, "dxgi.dll") || start_in_module(start, "d3d10core.dll") ||
        start_in_module(start, "d3d9.dll"))
        return ROLE_DXVK;
    return ROLE_OTHER;
}

static bool eligible(const Slot &s)
{
    if (s.exited || s.failed) return false;
    if (s.prio == 0) return true;
    if (g_po.extra_threads == 1) return s.role == ROLE_EXE;
    return g_po.extra_threads >= 2;
}

static int find_record(DWORD tid)
{
    for (LONG i = 0; i < g_nslots; i++)
        if (g_slots[i].tid == tid && !g_slots[i].exited)
            return (int)i;
    return -1;
}

static int new_record(int64_t now)
{
    if (g_nslots < MAX_REC)
        return (int)g_nslots;   // published by the caller
    for (int i = 0; i < MAX_REC; i++)
        if (g_slots[i].exited && !g_slots[i].active && now - g_slots[i].exitedAt > g_qpcFreq * 20)
            return i;
    return -1;
}

static void name_record(Slot &s, const char *desc)
{
    char sym[96] = "";
    if (s.start)
        symbolize(s.start, sym, sizeof(sym), NULL);
    if (desc[0])
        _snprintf(s.name, sizeof(s.name), "'%s' @ %s", desc, s.start ? sym : "?");
    else
        _snprintf(s.name, sizeof(s.name), "@ %s", s.start ? sym : "?");
    s.name[sizeof(s.name) - 1] = 0;
}

static void deactivate(Slot &s)
{
    if (!s.active)
        return;
    s.active = 0;
    if (s.h) {
        CloseHandle(s.h);
        s.h = NULL;
    }
}

// Lowest address of the committed, accessible run of regions that ends at
// [base, top), within one allocation and not below floor.
static uint32_t committed_floor(uint32_t base, const void *allocBase, uint32_t floor)
{
    MEMORY_BASIC_INFORMATION mbi;
    uint32_t lo = base;
    while (lo > floor + 0x1000 && base - lo < 0x400000) {
        if (!VirtualQuery((void *)(uintptr_t)(lo - 1), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || mbi.AllocationBase != allocBase)
            break;
        lo = (uint32_t)(uintptr_t)mbi.BaseAddress;
    }
    return lo < floor ? floor : lo;
}

// Open the thread, read its TEB and the kernel stack region of the Wine
// syscall frame (TEB fields are read with the thread suspended).
static bool activate(Slot &s)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                          FALSE, s.tid);
    if (!h) {
        s.failed = true;
        LOG(1, "profiler: OpenThread %lu (%s) failed, err %lu", s.tid, kRoleName[s.role],
            GetLastError());
        return false;
    }
    struct { LONG status; void *teb; void *pid, *tid; ULONG_PTR aff; LONG prio, base; } tbi;
    memset(&tbi, 0, sizeof(tbi));
    if (!g_ntqit || g_ntqit(h, 0 /* ThreadBasicInformation */, &tbi, sizeof(tbi), NULL) < 0 ||
        !tbi.teb) {
        s.failed = true;
        LOG(1, "profiler: no TEB for thread %lu (%s), not sampled", s.tid, kRoleName[s.role]);
        CloseHandle(h);
        return false;
    }
    const uint8_t *teb = (const uint8_t *)tbi.teb;
    uint32_t frame = 0, stackLimit = 0, stackBase = 0;
    if (SuspendThread(h) != (DWORD)-1) {
        frame = *(const volatile uint32_t *)(teb + TEB_SYSCALL_FRAME);
        stackBase = *(const volatile uint32_t *)(teb + 4);
        stackLimit = *(const volatile uint32_t *)(teb + 8);
        ResumeThread(h);
    }
    s.kLo = s.kHi = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (frame && !(frame >= stackLimit && frame < stackBase) &&
        VirtualQuery((void *)(uintptr_t)(frame - 4), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        uint32_t lo = (uint32_t)(uintptr_t)mbi.BaseAddress;
        uint32_t hi = lo + (uint32_t)mbi.RegionSize;
        if (frame > lo && frame + 0x10 <= hi) {
            s.kLo = committed_floor(lo, mbi.AllocationBase, 0);
            s.kHi = hi;
        }
    }
    s.sLo = s.sHi = 0;
    uint32_t tb = (uint32_t)(uintptr_t)teb;
    uint32_t blockEnd = (tb & ~0xFFFFu) + 0x10000;
    if (s.kHi && blockEnd - tb > 0x2000 &&
        VirtualQuery((void *)(uintptr_t)(blockEnd - 4), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT &&
        !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        uint32_t lo = (uint32_t)(uintptr_t)mbi.BaseAddress;
        uint32_t hi = lo + (uint32_t)mbi.RegionSize;
        lo = committed_floor(lo, mbi.AllocationBase, tb + 0x1000);
        if (lo < tb + 0x1000) lo = tb + 0x1000;
        if (hi > blockEnd) hi = blockEnd;
        if (hi > lo + 0x200) {
            s.sLo = lo;
            s.sHi = hi;
        }
    }
    s.h = h;
    s.teb = teb;
    BARRIER();
    s.active = 1;
    LOG(1, "profiler: sampling thread %lu as %s %s (record %d, kernel stack %08lx..%08lx, signal "
           "stack %08lx..%08lx)", s.tid, kRoleName[s.role], s.name, (int)(&s - g_slots),
        (unsigned long)s.kLo, (unsigned long)s.kHi, (unsigned long)s.sLo, (unsigned long)s.sHi);
    return true;
}

// Sampler thread, nothing suspended. Builds the sampled set: core threads
// first, then the exe's own threads, then DXVK threads, then the rest (as
// profiler_extra_threads allows), at most MAX_ACTIVE.
static void refresh_threads(void)
{
    int64_t now = qpc();
    for (LONG i = 0; i < g_nslots; i++)
        g_slots[i].seen = false;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        DWORD pid = GetCurrentProcessId();
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid)
                continue;
            DWORD tid = te.th32ThreadID;
            if (tid == g_samplerTid || tid == g_writerTid)
                continue;
            int r = find_record(tid);
            if (r >= 0) {
                Slot &s = g_slots[r];
                s.seen = true;
                // The render thread may be learned from Present after its record exists.
                if (g_renderTid && tid == (DWORD)g_renderTid && s.role != ROLE_RENDER) {
                    s.role = ROLE_RENDER;
                    s.prio = 0;
                }
                continue;
            }
            int idx = new_record(now);
            if (idx < 0)
                continue;
            Slot &s = g_slots[idx];
            memset(&s, 0, sizeof(Slot));
            s.tid = tid;
            s.start = thread_start(tid);
            char desc[64];
            thread_desc(tid, desc, sizeof(desc));
            s.role = classify(tid, s.start, desc);
            s.prio = kRolePrio[s.role];
            name_record(s, desc);
            s.seen = true;
            BARRIER();
            if (idx == g_nslots)
                g_nslots = idx + 1;
        }
        CloseHandle(snap);
    }
    // Fallback for the async worker when start addresses are unavailable.
    if (!g_startAddrWorks) {
        unsigned char *dm = (unsigned char *)*g_dmPtr;
        if (dm) {
            HANDLE wh = *(HANDLE volatile *)(dm + DM_WORKER_HANDLE);
            DWORD wt = wh ? GetThreadId(wh) : 0;
            int r = wt ? find_record(wt) : -1;
            if (r >= 0 && g_slots[r].prio != 0) {
                g_slots[r].role = ROLE_ASYNC;
                g_slots[r].prio = 0;
            }
        }
    }
    // Exited threads.
    for (LONG i = 0; i < g_nslots; i++) {
        Slot &s = g_slots[i];
        if (!s.exited && !s.seen) {
            s.exited = true;
            s.exitedAt = now;
            if (s.active)
                LOG(1, "profiler: thread %lu (%s) exited", s.tid, kRoleName[s.role]);
            deactivate(s);
        }
    }
    // Choose the sampled set: by priority, keeping already active threads on ties.
    int cand[MAX_REC];
    int nc = 0;
    for (LONG i = 0; i < g_nslots; i++)
        if (eligible(g_slots[i]))
            cand[nc++] = (int)i;
    for (int i = 1; i < nc; i++) {        // insertion sort, nc <= 256
        int c = cand[i], j = i - 1;
        while (j >= 0) {
            const Slot &a = g_slots[cand[j]], &b = g_slots[c];
            bool after = a.prio > b.prio || (a.prio == b.prio && !a.active && b.active) ||
                         (a.prio == b.prio && a.active == b.active && cand[j] > c);
            if (!after) break;
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1] = c;
    }
    // With every thread eligible, the game's own extra threads (job and IO
    // workers, mostly idle) take at most MAX_EXE_MODE2 slots so DXVK's
    // threads get the rest.
    bool want[MAX_REC];
    memset(want, 0, sizeof(want));
    int picked = 0, exe = 0, kept = 0;
    for (int i = 0; i < nc && picked < MAX_ACTIVE; i++) {
        const Slot &s = g_slots[cand[i]];
        if (s.role == ROLE_EXE && g_po.extra_threads >= 2 && exe >= MAX_EXE_MODE2)
            continue;
        exe += s.role == ROLE_EXE;
        want[cand[i]] = true;
        cand[kept++] = cand[i];
        picked++;
    }
    nc = kept;
    for (LONG i = 0; i < g_nslots; i++)
        if (g_slots[i].active && !want[i])
            deactivate(g_slots[i]);
    int na = 0;
    for (int i = 0; i < nc && na < MAX_ACTIVE; i++) {
        Slot &s = g_slots[cand[i]];
        if (s.active || activate(s))
            g_active[na++] = cand[i];
    }
    g_nactive = na;
}

// ---------------------------------------------------------------- sampling

// The suspended window: SuspendThread .. ResumeThread. Only kernel32 thread
// calls, QueryPerformanceCounter and plain memory reads (target stack, TEB,
// Wine kernel stack) in between.
static void sample_slot(int i)
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
        smp->nn = 0;
        smp->sys = 0;
        smp->neip = 0;
        uint32_t esp = ctx.Esp;
        uint32_t stackBase = *(const volatile uint32_t *)(s.teb + 4);
        uint32_t stackLimit = *(const volatile uint32_t *)(s.teb + 8);
        if (esp >= stackLimit && esp < stackBase && (esp & 3) == 0) {
            uint32_t bytes = stackBase - esp;
            if (bytes > STACK_SCAN_BYTES)
                bytes = STACK_SCAN_BYTES;
            smp->n = (uint8_t)prof_scan_stack((const uint32_t *)(uintptr_t)esp, bytes / 4,
                                              g_ranges, g_nranges, smp->ra, MAX_RA);
        } else {
            s.noStack++;
        }
        // Inside a syscall / unix call: the context is the one saved in the
        // Wine syscall frame. The live native stack runs from the native ESP
        // (signal frame) up to the syscall frame; scan it innermost first for
        // addresses in native executable mappings.
        uint32_t fr = *(const volatile uint32_t *)(s.teb + TEB_SYSCALL_FRAME);
        if (s.kHi && fr > s.kLo && fr + 0x10 <= s.kHi && (fr & 3) == 0 &&
            *(const volatile uint32_t *)(uintptr_t)(fr + FRAME_EIP) == ctx.Eip &&
            *(const volatile uint32_t *)(uintptr_t)(fr + FRAME_ESP) == ctx.Esp) {
            smp->sys = 1;
            s.inSys++;
            // Interrupted native registers from the signal frame, newest
            // (highest) first; ESP must lie on the kernel stack below fr.
            uint32_t nesp = 0;
            if (s.sHi) {
                uint32_t lo = s.sHi - s.sLo > SIG_SCAN_BYTES ? s.sHi - SIG_SCAN_BYTES : s.sLo;
                for (uint32_t a = s.sHi - 19 * 4; a >= lo; a -= 4) {
                    const volatile uint32_t *g = (const volatile uint32_t *)(uintptr_t)a;
                    if (g[15] == 0x23 && g[2] == 0x2b && g[3] == 0x2b && g[18] == 0x2b &&
                        g[7] == g[17]) {
                        if (g[7] >= s.kLo && g[7] < fr) {
                            nesp = g[7];
                            smp->neip = g[14];
                        }
                        break;
                    }
                }
            }
            if (!nesp)
                s.noSig++;
            if (nesp && g_nnatRanges) {
                uint32_t hi = fr - nesp > KSTACK_SCAN_BYTES ? nesp + KSTACK_SCAN_BYTES : fr;
                uint32_t minLo = g_natRanges[0].lo, maxHi = g_natRanges[g_nnatRanges - 1].hi;
                int k = 0;
                for (uint32_t a = nesp & ~3u; a < hi && k < MAX_NRA; a += 4) {
                    uint32_t w = *(const volatile uint32_t *)(uintptr_t)a;
                    if (w < minLo || w >= maxHi || (k && smp->nra[k - 1] == w))
                        continue;
                    if (prof_find_range(g_natRanges, g_nnatRanges, w) >= 0)
                        smp->nra[k++] = w;
                }
                smp->nn = (uint8_t)k;
            }
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
    char thr[2400];
    int k = 0;
    thr[0] = 0;
    for (int j = 0; j < g_nactive && k < (int)sizeof(thr) - 160; j++) {
        const Slot &s = g_slots[g_active[j]];
        LONG tot = s.samples + s.fails;
        int m = _snprintf(thr + k, sizeof(thr) - k - 1,
                          "%s%s %lu: %ld ok %ld fail %ld sys %ld nosig, susp avg %ld us max %ld us",
                          k ? "; " : "", kRoleName[s.role], s.tid, (long)s.samples, (long)s.fails,
                          (long)s.inSys, (long)s.noSig, (long)(tot ? ticks_to_us(s.suspTicks / tot) : 0),
                          (long)ticks_to_us(s.suspMax));
        if (m < 0) break;
        k += m;
    }
    thr[sizeof(thr) - 1] = 0;
    LONG ticks = g_ticks;
    LOG(1, "profiler: ticks %ld, avg tick %ld us, max tick %ld us, frames %ld, max frame %ld.%03ld ms, "
           "hitches %ld, dumps queued %ld written %ld dropped %ld, readfile %ld, threads seen %ld, "
           "sampled %d: %s",
        (long)ticks, (long)(ticks ? ticks_to_us(g_tickTotal / ticks) : 0),
        (long)ticks_to_us(g_tickMax), (long)g_frames, (long)(g_maxFrameUs / 1000),
        (long)(g_maxFrameUs % 1000), (long)g_hitches, (long)g_dumpsQueued, (long)g_dumpsWritten,
        (long)g_reqDropped, (long)g_rfCalls, (long)g_nslots, g_nactive, thr);
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
    refresh_native();
    refresh_threads();
    LOG(1, "profiler: sampler running at %d Hz, ring %u samples (%d ms), %d code ranges in %d modules, "
           "%d native ranges, start addresses %s, extra_threads %d",
        g_po.hz, g_ringCap, g_po.ring_ms, g_nranges, g_nmods, g_nnatRanges,
        g_startAddrWorks ? "available" : "UNAVAILABLE (fallbacks only)", g_po.extra_threads);
    g_samplerRunning = 1;
    int64_t next = qpc();
    int64_t nextRefresh = next + g_qpcFreq * 2;
    int64_t nextStatus = next + g_qpcFreq * 10;
    bool maxLogged = false;
    for (;;) {
        int64_t t0 = qpc();
        if (t0 >= nextRefresh) {
            refresh_modules();
            refresh_native();
            refresh_threads();
            nextRefresh = qpc() + g_qpcFreq * 2;
        }
        int64_t ts = qpc();
        for (int j = 0; j < g_nactive; j++)
            if (g_slots[g_active[j]].active)
                sample_slot(g_active[j]);
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

// Native return addresses of the thread's samples: histogram by mapping name
// (once per sample) and by address.
static void native_name(uint32_t a, char *nm, size_t n)
{
    EnterCriticalSection(&g_modCs);
    const NatRange *r = find_native(a);
    if (r) { strncpy(nm, r->name, n - 1); nm[n - 1] = 0; } else strcpy(nm, "?");
    LeaveCriticalSection(&g_modCs);
}

static void dump_native(Buf *b, int slot, const Sample *smp, uint32_t n, uint32_t cnt, int top)
{
    uint32_t sys = 0, total = 0, withLeaf = 0;
    for (uint32_t i = 0; i < n; i++)
        if (smp[i].slot == slot && smp[i].sys) {
            sys++;
            total += smp[i].nn;
            withLeaf += smp[i].neip != 0;
        }
    if (!sys)
        return;
    bput(b, "  inside syscalls / unix calls: %u of %u samples (%.1f%%), native registers found in %u\n",
         sys, cnt, 100.0 * sys / cnt, withLeaf);
    char sym[200];
    if (withLeaf) {
        // Native leaf: the interrupted native EIP, by address and by file.
        uint32_t *la = (uint32_t *)malloc(sizeof(uint32_t) * withLeaf);
        KC *lc = (KC *)malloc(sizeof(KC) * withLeaf);
        uint32_t q = 0;
        for (uint32_t i = 0; i < n; i++)
            if (smp[i].slot == slot && smp[i].sys && smp[i].neip)
                la[q++] = smp[i].neip;
        const int MAXF = 32;
        char fn[MAXF][44];
        uint32_t fc[MAXF];
        int nf = 0;
        for (uint32_t i = 0; i < q; i++) {
            char nm[44];
            native_name(la[i], nm, sizeof(nm));
            int k = 0;
            while (k < nf && strcmp(fn[k], nm) != 0) k++;
            if (k == nf) {
                if (nf == MAXF) continue;
                memcpy(fn[nf], nm, sizeof(nm));
                fc[nf++] = 0;
            }
            fc[k]++;
        }
        bput(b, "  native leaf by file (samples, pct of syscall samples, file):\n");
        for (int pass = 0; pass < nf && pass < top; pass++) {
            int best = -1;
            for (int k = 0; k < nf; k++)
                if (fc[k] && (best < 0 || fc[k] > fc[best])) best = k;
            if (best < 0) break;
            bput(b, "    %5u %5.1f%%  %s\n", fc[best], 100.0 * fc[best] / sys, fn[best]);
            fc[best] = 0;
        }
        qsort(la, q, sizeof(uint32_t), u32_cmp);
        int nl = count_runs(la, (int)q, lc);
        bput(b, "  native leaf addresses (samples, address, file+offset):\n");
        for (int i = 0; i < nl && i < top; i++) {
            symbolize(lc[i].k, sym, sizeof(sym), NULL);
            bput(b, "    %5u  %08x  %s\n", lc[i].c, lc[i].k, sym);
        }
        free(lc);
        free(la);
    }
    if (!total)
        return;
    // By mapping name.
    const int MAXN = 48;
    char names[MAXN][44];
    uint32_t ncount[MAXN];
    int nn = 0;
    uint32_t *addrs = (uint32_t *)malloc(sizeof(uint32_t) * total);
    KC *ac = (KC *)malloc(sizeof(KC) * total);
    uint32_t na = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (smp[i].slot != slot || !smp[i].sys)
            continue;
        int seenIdx[MAX_NRA];
        int ns = 0;
        for (int j = 0; j < smp[i].nn; j++) {
            uint32_t a = smp[i].nra[j];
            addrs[na++] = a;
            char nm[44];
            native_name(a, nm, sizeof(nm));
            int k = 0;
            while (k < nn && strcmp(names[k], nm) != 0)
                k++;
            if (k == nn) {
                if (nn == MAXN) continue;
                memcpy(names[nn], nm, sizeof(nm));
                ncount[nn] = 0;
                nn++;
            }
            bool dup = false;
            for (int q = 0; q < ns; q++)
                if (seenIdx[q] == k) dup = true;
            if (!dup) {
                seenIdx[ns++] = k;
                ncount[k]++;
            }
        }
    }
    bput(b, "  native files on the live native stack (samples, pct of syscall samples, file):\n");
    for (int pass = 0; pass < nn && pass < top; pass++) {
        int best = -1;
        for (int k = 0; k < nn; k++)
            if (ncount[k] && (best < 0 || ncount[k] > ncount[best]))
                best = k;
        if (best < 0) break;
        bput(b, "    %5u %5.1f%%  %s\n", ncount[best], 100.0 * ncount[best] / sys, names[best]);
        ncount[best] = 0;
    }
    qsort(addrs, na, sizeof(uint32_t), u32_cmp);
    int nr = count_runs(addrs, (int)na, ac);
    bput(b, "  native return addresses (samples, address, file+offset):\n");
    for (int i = 0; i < nr && i < top; i++) {
        symbolize(ac[i].k, sym, sizeof(sym), NULL);
        bput(b, "    %5u  %08x  %s\n", ac[i].c, ac[i].k, sym);
    }
    free(ac);
    free(addrs);
}

static void dump_thread(Buf *b, int slot, const Sample *smp, uint32_t n, bool stacks, bool compact)
{
    const Slot &s = g_slots[slot];
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n; i++)
        cnt += smp[i].slot == slot;
    if (!cnt)
        return;
    bput(b, "\n-- thread %s (tid %lu) %s: %u samples\n", kRoleName[s.role], s.tid, s.name, cnt);
    char sym[200];
    int topLeaf = compact ? 6 : 20, topRa = compact ? 10 : 30;

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
    for (int i = 0; i < nk && i < topLeaf; i++) {
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
    for (int i = 0; i < nr && i < topRa; i++) {
        symbolize(rc[i].k, sym, sizeof(sym), NULL);
        bput(b, "    %5u %5.1f%%  %08x  %s\n", rc[i].c, 100.0 * rc[i].c / cnt, rc[i].k, sym);
    }

    dump_native(b, slot, smp, n, cnt, compact ? 8 : 16);

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
        KC *grp = (KC *)malloc(sizeof(KC) * (q ? q : 1));
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
        bput(b, "  top stacks (leaf first, then return addresses; native frames of the sample last):\n");
        for (int g = 0; g < ng && g < 5; g++) {
            const Sample &x = smp[grp[g].k];
            symbolize(x.eip, sym, sizeof(sym), NULL);
            bput(b, "    #%d: %u samples (%.1f%%)\n      %08x  %s\n", g + 1, grp[g].c,
                 100.0 * grp[g].c / cnt, x.eip, sym);
            for (int j = 0; j < x.n; j++) {
                symbolize(x.ra[j], sym, sizeof(sym), NULL);
                bput(b, "      %08x  %s\n", x.ra[j], sym);
            }
            if (x.neip) {
                symbolize(x.neip, sym, sizeof(sym), NULL);
                bput(b, "      native leaf %08x  %s\n", x.neip, sym);
            }
            for (int j = 0; j < x.nn; j++) {
                symbolize(x.nra[j], sym, sizeof(sym), NULL);
                bput(b, "      native %08x  %s\n", x.nra[j], sym);
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

// One line per sampled thread: samples, share inside syscalls, top leaf.
static void dump_summary(Buf *b, const Sample *smp, uint32_t n)
{
    bput(b, "\nthreads (role, tid, samples, pct in syscall or unix call, top leaf, name):\n");
    LONG ns = g_nslots;
    for (int o = 0; o <= ROLE_OTHER; o++)
        for (LONG i = 0; i < ns; i++) {
            if (g_slots[i].role != o)
                continue;
            uint32_t cnt = 0, sys = 0;
            for (uint32_t k = 0; k < n; k++)
                if (smp[k].slot == i) {
                    cnt++;
                    sys += smp[k].sys;
                }
            if (!cnt)
                continue;
            // Most frequent leaf function (small quadratic count, cnt <= ~1000).
            uint32_t bestKey = 0, bestC = 0;
            uint32_t *keys = (uint32_t *)malloc(sizeof(uint32_t) * cnt);
            uint32_t m = 0;
            char sym[200];
            for (uint32_t k = 0; k < n; k++)
                if (smp[k].slot == i) {
                    uint32_t key;
                    symbolize(smp[k].eip, sym, sizeof(sym), &key);
                    keys[m++] = key;
                }
            qsort(keys, m, sizeof(uint32_t), u32_cmp);
            for (uint32_t k = 0; k < m;) {
                uint32_t j = k;
                while (j < m && keys[j] == keys[k]) j++;
                if (j - k > bestC) { bestC = j - k; bestKey = keys[k]; }
                k = j;
            }
            free(keys);
            symbolize(bestKey, sym, sizeof(sym), NULL);
            bput(b, "  %-12s %6lu %5u %5.1f%%  %5.1f%% %s  %s\n", kRoleName[g_slots[i].role],
                 g_slots[i].tid, cnt, 100.0 * sys / cnt, 100.0 * bestC / cnt, sym, g_slots[i].name);
        }
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
        const char *who = "not sampled";
        int ri = find_record(agg[a].tid);
        if (ri >= 0) who = kRoleName[g_slots[ri].role];
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
    if (dxhrfix_hooks_format_cursor(line, sizeof(line)))
        bput(b, "%s\n", line);
    bput(b, "profiler: frames %ld, hitches %ld, readfile calls %ld\n", (long)g_frames,
         (long)g_hitches, (long)g_rfCalls);
    dump_summary(b, g_snap, n);
    LONG ns = g_nslots;
    // Render and main first, with stacks; then the others by role.
    static const int order[] = {ROLE_RENDER, ROLE_MAIN, ROLE_ASYNC, ROLE_FS, ROLE_EXE, ROLE_DXVK,
                                ROLE_OTHER};
    for (int o = 0; o < 7; o++)
        for (LONG i = 0; i < ns; i++)
            if (g_slots[i].role == order[o])
                dump_thread(b, (int)i, g_snap, n, order[o] == ROLE_RENDER || order[o] == ROLE_MAIN,
                            kRolePrio[order[o]] > 0);
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
        applied += install_readfile_hook();
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
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    if (!kb) kb = GetModuleHandleW(L"kernel32.dll");
    g_getDesc = kb ? (GetThreadDescription_fn)(void *)GetProcAddress(kb, "GetThreadDescription") : NULL;
    g_mapsBuf = (char *)VirtualAlloc(NULL, MAPS_BUF, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    load_symbols();

    g_ringCap = (uint32_t)((int64_t)g_po.hz * g_po.ring_ms / 1000) * MAX_ACTIVE;
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
