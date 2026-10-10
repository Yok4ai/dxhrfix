// DFEngine.dll proxy for DXHRDC.exe (Deus Ex HR Director's Cut, Steam 238010).
//
// DllMain PROCESS_ATTACH reads dxhrfix.ini, opens dxhrfix.log, byte-checks
// the hook sites and (top-level build, -DDXHRFIX_HOOKS, hooks=1) installs
// the src/ hooks. The first GetDFEngine() call (outside loader lock) logs
// exe size and sha256, then forwards to DFEngine_orig.dll or returns a no-op
// stub object. The proxy/ Makefile builds the stage 1 variant (no patching).
//
// How the exe uses the DFEngine object (from DXHRDC.objdump.txt):
//   GetDFEngine() takes no args, returns the object pointer (call site 0x628260
//   region, manager singleton 0x1C78C84, object stored at manager+4).
//   Every vtable method is __stdcall with `this` as first stack arg.
//   slot  1 (+0x04) Start(this, 0x04050000, cfg*) -> 0 ok, else the game drops
//                   the object (manager+4 = 0) and never touches it again.
//   slot  2 (+0x08) Shutdown(this)                       0x628479
//   slot  4 (+0x10) CreatePlacement(this, a, b, desc*, out**)  0x6F014E
//   slot  5 (+0x14) DestroyPlacement(this, placement)    0x6EFF6C
//   slot  6 (+0x18) (this, 0) "end session"              0x628465
//   slot  7 (+0x1C) (this, zoneName) RefreshCreatives    0x6281BE, 0x62850C
//   slot  8 (+0x20) Update(this), once per frame         0x6284C7, 0x628082
//   slot  9 (+0x24) (this, zoneName) StartZone          0x6283D6, 0x628193
//   slot 10 (+0x28) (this, zoneName) EndZone             0x62816D
//   slot 14 (+0x38) (this, matrix*) SetView              0x6280DB
//   slot 15 (+0x3C) (this, matrix*) SetProjection        0x628106
//   slot 18 (+0x48) (this, bool)                         0x628451, 0x6284DA
// Every caller null-checks manager+4, and the placement objects (returned via
// slot 4) are only reachable when Start succeeded. The stub therefore fails
// Start with E_FAIL, which takes the game's own "ad SDK unavailable" path; the
// other slots are still implemented with correct stdcall arg counts as a
// safety net.

#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifdef DXHRFIX_HOOKS
#include "../src/dxhrfix_hooks.h"
#define PROXY_VERSION "0.2.0-hookA"
#define DEFAULT_HOOKS 1
#define DEFAULT_PRECREATE 1
#else
#define DEFAULT_PRECREATE 0
#define PROXY_VERSION "0.1.0"
#define DEFAULT_HOOKS 0
#endif

static HMODULE g_self;
static volatile LONG g_initState; // 0 none, 1 running, 2 done
static void *g_engine;

struct Config {
    int hooks;
    int precreate;
    int frame_latency;
    int log_level;
    int ad_sdk; // 1 = forward to DFEngine_orig.dll when present, 0 = always stub
    int pool_threads;          // 0 = auto
    int precreate_max_per_sec; // 0 = unlimited
};
static Config g_cfg = {DEFAULT_HOOKS, DEFAULT_PRECREATE, 2, 1, 1, 0, 0};

static wchar_t g_exeDir[MAX_PATH];
static wchar_t g_exePath[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_logLock;
static volatile LONG g_exiting; // DLL_PROCESS_DETACH: other threads may be dead, log nothing
static volatile LONG g_flushAll = 1; // flush every line only during init and detach (an fsync under Wine)

// ---------------------------------------------------------------- logging

static void dlogv(int level, const char *fmt, va_list ap)
{
    if (g_log == INVALID_HANDLE_VALUE || level > g_cfg.log_level || g_exiting)
        return;   // nothing is formatted during DLL_PROCESS_DETACH
    char buf[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = _snprintf(buf, sizeof(buf) - 2, "%02u:%02u:%02u.%03u [%lu] ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                      GetCurrentThreadId());
    if (n < 0) n = 0;
    int m = _vsnprintf(buf + n, sizeof(buf) - 2 - n, fmt, ap);
    if (m < 0) m = (int)(sizeof(buf) - 2 - n);
    n += m;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_logLock);
    DWORD w;
    WriteFile(g_log, buf, (DWORD)n, &w, NULL);
    if (g_flushAll)
        FlushFileBuffers(g_log);
    LeaveCriticalSection(&g_logLock);
}

static void dlog(int level, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dlogv(level, fmt, ap);
    va_end(ap);
}

static HANDLE open_log(const wchar_t *path)
{
    return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void init_log(void)
{
    wchar_t path[MAX_PATH + 32];
    _snwprintf(path, MAX_PATH + 31, L"%ls\\dxhrfix.log", g_exeDir);
    path[MAX_PATH + 31] = 0;
    g_log = open_log(path);
    if (g_log == INVALID_HANDLE_VALUE) {
        wchar_t la[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            _snwprintf(path, MAX_PATH + 31, L"%ls\\dxhrfix.log", la);
            path[MAX_PATH + 31] = 0;
            g_log = open_log(path);
        }
    }
    if (g_log != INVALID_HANDLE_VALUE) {
        char p8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, p8, sizeof(p8), NULL, NULL);
        dlog(0, "dxhrfix proxy %s, log %s", PROXY_VERSION, p8);
    }
}

// ---------------------------------------------------------------- config

static int ini_int(const wchar_t *ini, const wchar_t *key, int def)
{
    return (int)GetPrivateProfileIntW(L"dxhrfix", key, def, ini);
}

static void load_config(void)
{
    wchar_t ini[MAX_PATH + 32];
    _snwprintf(ini, MAX_PATH + 31, L"%ls\\dxhrfix.ini", g_exeDir);
    ini[MAX_PATH + 31] = 0;
    g_cfg.hooks = ini_int(ini, L"hooks", g_cfg.hooks);
    g_cfg.precreate = ini_int(ini, L"precreate", g_cfg.precreate);
    g_cfg.frame_latency = ini_int(ini, L"frame_latency", g_cfg.frame_latency);
    g_cfg.log_level = ini_int(ini, L"log_level", g_cfg.log_level);
    g_cfg.ad_sdk = ini_int(ini, L"ad_sdk", g_cfg.ad_sdk);
    g_cfg.pool_threads = ini_int(ini, L"pool_threads", g_cfg.pool_threads);
    g_cfg.precreate_max_per_sec = ini_int(ini, L"precreate_max_per_sec", g_cfg.precreate_max_per_sec);
    if (g_cfg.frame_latency < 1) g_cfg.frame_latency = 1;
    if (g_cfg.frame_latency > 4) g_cfg.frame_latency = 4;
}

// ---------------------------------------------------------------- exe info

static void hex(const unsigned char *p, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 3] = d[p[i] >> 4];
        out[i * 3 + 1] = d[p[i] & 15];
        out[i * 3 + 2] = ' ';
    }
    out[n ? n * 3 - 1 : 0] = 0;
}

static void log_exe_info(void)
{
    HANDLE f = CreateFileW(g_exePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        dlog(0, "exe open failed, err %lu", GetLastError());
        return;
    }
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz))
        dlog(0, "exe size %lld bytes", (long long)sz.QuadPart);

    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        static unsigned char buf[1 << 16];
        DWORD r;
        while (ReadFile(f, buf, sizeof(buf), &r, NULL) && r)
            BCryptHashData(h, buf, r, 0);
        unsigned char dig[32];
        if (BCryptFinishHash(h, dig, 32, 0) == 0) {
            char s[65];
            for (int i = 0; i < 32; i++)
                sprintf(s + i * 2, "%02x", dig[i]);
            const char *stock = "8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1";
            dlog(0, "exe sha256 %s (%s)", s, strcmp(s, stock) ? "NOT stock" : "stock");
        }
    } else {
        dlog(1, "sha256 unavailable (bcrypt)");
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
}

// ---------------------------------------------------------------- hook sites

struct HookSite {
    const char *name;
    DWORD va; // VA at preferred base 0x400000
    unsigned char bytes[8];
    int len;
};

static const HookSite kSites[] = {
    {"awaitResource entry", 0x54C800, {0x56, 0x8b, 0xf1, 0x8a, 0x46, 0x08, 0x84, 0xc0}, 8},
    {"awaitResource Sleep(1) push", 0x54C811, {0x6a, 0x01}, 2},
    {"awaitResource twin Sleep(1) push", 0x54C877, {0x6a, 0x01}, 2},
    {"cancelOrAwait entry", 0x54C850, {0x56, 0x8b, 0xf1, 0x80, 0x7e, 0x09, 0x00, 0x74}, 8},
    {"SetMaximumFrameLatency(1) call", 0x54AB80, {0x8b, 0x51, 0x30, 0x6a, 0x01, 0x50, 0xff, 0xd2}, 8},
};

static bool readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return false;
    DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
               PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok) || (mbi.Protect & PAGE_GUARD))
        return false;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

static int check_hook_sites(unsigned char *base)
{
    int good = 0;
    const int count = (int)(sizeof(kSites) / sizeof(kSites[0]));
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    DWORD imageSize = nt->OptionalHeader.SizeOfImage;
    dlog(0, "exe SizeOfImage 0x%lX, timestamp 0x%08lX", imageSize, nt->FileHeader.TimeDateStamp);
    for (int i = 0; i < count; i++) {
        const HookSite &s = kSites[i];
        unsigned char *p = base + (s.va - 0x400000);
        char want[32], got[32];
        hex(s.bytes, s.len, want);
        if (s.va - 0x400000 + s.len > imageSize || !readable(p, s.len)) {
            dlog(0, "site %-34s VA 0x%06lX @%p UNREADABLE", s.name, s.va, p);
            continue;
        }
        hex(p, s.len, got);
        bool match = memcmp(p, s.bytes, s.len) == 0;
        good += match;
        dlog(0, "site %-34s VA 0x%06lX @%p %s (want %s, got %s)", s.name, s.va, p,
             match ? "MATCH" : "MISMATCH", want, got);
    }
    dlog(0, "hook sites: %d/%d match", good, count);
    return good;
}

// ---------------------------------------------------------------- stub engine

#define STUB_FAIL ((int)0x80004005) // E_FAIL

static int __stdcall st_this(void *) { return 0; }
static int __stdcall st_start(void *, unsigned ver, void *)
{
    dlog(1, "stub Start(ver 0x%08X) -> E_FAIL (ad SDK disabled)", ver);
    return STUB_FAIL;
}
static int __stdcall st_create_placement(void *, int, int, void *, void **out)
{
    if (out) *out = NULL;
    return STUB_FAIL;
}
static int __stdcall st_1arg(void *, void *) { return 0; }

// Unknown slots get st_this; only slots 1..18 are known to be called.
static void *g_stubVtbl[32];
static struct { void **vtbl; } g_stub = {g_stubVtbl};

static void build_stub(void)
{
    for (int i = 0; i < 32; i++)
        g_stubVtbl[i] = (void *)st_this;
    g_stubVtbl[1] = (void *)st_start;
    g_stubVtbl[2] = (void *)st_this;            // Shutdown
    g_stubVtbl[4] = (void *)st_create_placement;
    g_stubVtbl[5] = (void *)st_1arg;            // DestroyPlacement
    g_stubVtbl[6] = (void *)st_1arg;
    g_stubVtbl[7] = (void *)st_1arg;            // RefreshCreatives(zone)
    g_stubVtbl[8] = (void *)st_this;            // Update
    g_stubVtbl[9] = (void *)st_1arg;            // StartZone(zone)
    g_stubVtbl[10] = (void *)st_1arg;           // EndZone(zone)
    g_stubVtbl[14] = (void *)st_1arg;           // SetView
    g_stubVtbl[15] = (void *)st_1arg;           // SetProjection
    g_stubVtbl[18] = (void *)st_1arg;
}

// ---------------------------------------------------------------- init

typedef void *(__cdecl *GetDFEngine_t)(void);

static void *resolve_engine(void)
{
    if (!g_cfg.ad_sdk) {
        dlog(0, "ad_sdk=0: using stub engine");
        return &g_stub;
    }
    wchar_t path[MAX_PATH + 32];
    _snwprintf(path, MAX_PATH + 31, L"%ls\\DFEngine_orig.dll", g_exeDir);
    path[MAX_PATH + 31] = 0;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        dlog(0, "DFEngine_orig.dll not found: using stub engine");
        return &g_stub;
    }
    HMODULE m = LoadLibraryW(path);
    if (!m || m == g_self) {
        dlog(0, "LoadLibrary DFEngine_orig.dll failed (mod %p, err %lu): stub", m, GetLastError());
        return &g_stub;
    }
    GetDFEngine_t fn = (GetDFEngine_t)(void *)GetProcAddress(m, "GetDFEngine");
    if (!fn) {
        dlog(0, "DFEngine_orig.dll lacks GetDFEngine: stub");
        return &g_stub;
    }
    void *e = fn();
    dlog(0, "forwarded to DFEngine_orig.dll @%p, engine %p vtbl %p", m, e,
         e ? *(void **)e : NULL);
    return e;
}

// Early part: runs from DllMain PROCESS_ATTACH, i.e. before the exe entry
// point and before any game thread exists (render thread, async-create
// worker). Only kernel32 calls here (no LoadLibrary, no bcrypt).
static void early_init(void)
{
    InitializeCriticalSection(&g_logLock);
    build_stub();
    DWORD n = GetModuleFileNameW(NULL, g_exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        g_exePath[0] = 0;
        g_exeDir[0] = L'.';
        g_exeDir[1] = 0;
    } else {
        wcscpy(g_exeDir, g_exePath);
        wchar_t *s = wcsrchr(g_exeDir, L'\\');
        if (s) *s = 0;
    }
    load_config();
    init_log();
    unsigned char *base = (unsigned char *)GetModuleHandleW(NULL);
    dlog(0, "exe base %p (preferred 0x400000), proxy base %p", base, g_self);
    dlog(0, "config: hooks=%d precreate=%d frame_latency=%d log_level=%d ad_sdk=%d "
            "pool_threads=%d precreate_max_per_sec=%d",
         g_cfg.hooks, g_cfg.precreate, g_cfg.frame_latency, g_cfg.log_level, g_cfg.ad_sdk,
         g_cfg.pool_threads, g_cfg.precreate_max_per_sec);
    check_hook_sites(base);
#ifdef DXHRFIX_HOOKS
    if (g_cfg.hooks) {
        DxhrfixOptions o;
        o.frame_latency = g_cfg.frame_latency;
        o.log_level = g_cfg.log_level;
        o.precreate = g_cfg.precreate;
        o.pool_threads = g_cfg.pool_threads;
        o.precreate_max_per_sec = g_cfg.precreate_max_per_sec;
        dxhrfix_hooks_install(base, &o, dlog);
    }
    else
        dlog(0, "hooks=0: stock behavior, nothing patched");
#else
    dlog(0, "stage 1 build: patching nothing");
#endif
    g_flushAll = 0; // init done: plain WriteFile from here on (data survives a kill)
}

// Late part: first GetDFEngine call, outside the loader lock.
static void do_init(void)
{
    log_exe_info();
    g_engine = resolve_engine();
    dlog(0, "GetDFEngine -> %p (%s)", g_engine, g_engine == &g_stub ? "stub" : "orig");
#ifdef DXHRFIX_HOOKS
    if (g_cfg.hooks)
        dxhrfix_hooks_start_threads();
#endif
}

extern "C" void *__cdecl GetDFEngine(void)
{
    if (InterlockedCompareExchange(&g_initState, 1, 0) == 0) {
        do_init();
        InterlockedExchange(&g_initState, 2);
    } else {
        while (g_initState != 2)
            Sleep(0);
    }
    return g_engine;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        early_init();
    } else if (reason == DLL_PROCESS_DETACH) {
        // Other threads may be dead holding the CRT or our log lock: log
        // nothing here (the 10 s reporter carries the totals).
        g_exiting = 1;
    }
    return TRUE;
}
