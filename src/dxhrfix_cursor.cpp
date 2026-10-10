// Cursor call dedupe for DXHRDC.exe (cursor_fix=1).
//
// The UI update (0x81A130, called every frame from the message pump
// 0x4A55F0 and from 0x4B03F0 / 0x81A491 / 0x81A4C1) runs
//   while (ShowCursor(TRUE) < 0) ;   or   while (ShowCursor(FALSE) >= 0) ;
// and then SetCursor(handle or NULL), and 0x4B07A0 (same pump) calls
// ClipCursor(rect or NULL) every frame. Under Wine each of these is a
// wineserver round trip and some reach the X server; a profiled 113 ms
// gameplay frame had 78% of the main thread's samples inside
// win32u!NtUserShowCursor under 0x81A1A4. Note also that the "show" loop
// adds 1 to the display count every visible frame, so the stock "hide" loop
// later makes one real call per frame the cursor was visible.
//
// IAT hooks (slots verified to hold the user32 export first):
//   ShowCursor 0xA813EC: a shadow display count. The first call goes to
//     user32 and seeds the shadow with its result. Later calls apply +1/-1
//     to the shadow and call user32 only when visibility (count >= 0)
//     changes; the shadow count is returned, so the game's loops end exactly
//     as they would with the real count. If user32's answer disagrees on
//     visibility after a real call, the shadow is resynced to it.
//   SetCursor 0xA813E8: user32 only when the handle differs from the last one
//     passed, or cursor_refresh_ms passed since the last real call. Returns
//     the previous handle.
//   ClipCursor 0xA81328: same, keyed on the rect (or NULL); returns TRUE when
//     skipped.
// Only the thread that first calls ShowCursor (the main thread) is deduped;
// other threads go straight to user32 (the display count is per thread
// input queue in Windows).

#include "dxhrfix_internal.h"

#include <stdio.h>
#include <string.h>

static const uint32_t VA_IAT_SHOWCURSOR = 0xA813EC;
static const uint32_t VA_IAT_SETCURSOR = 0xA813E8;
static const uint32_t VA_IAT_CLIPCURSOR = 0xA81328;

typedef int (WINAPI *ShowCursor_fn)(BOOL);
typedef HCURSOR (WINAPI *SetCursor_fn)(HCURSOR);
typedef BOOL (WINAPI *ClipCursor_fn)(const RECT *);

static ShowCursor_fn g_realShow;
static SetCursor_fn g_realSet;
static ClipCursor_fn g_realClip;

static volatile LONG g_ownerTid;
static int64_t g_refreshTicks;

static bool g_seeded;
static int g_shadow;

static bool g_haveCursor;
static HCURSOR g_lastCursor;
static int64_t g_lastSetReal;

static bool g_haveClip;
static bool g_lastClipNull;
static RECT g_lastClip;
static int64_t g_lastClipReal;

static volatile LONG g_showCalls, g_showReal, g_showResync, g_showOther;
static volatile LONG g_setCalls, g_setReal, g_setOther;
static volatile LONG g_clipCalls, g_clipReal, g_clipOther;
static volatile LONG g_showMaxShadow;

static bool owner_thread(void)
{
    LONG tid = (LONG)GetCurrentThreadId();
    if (!g_ownerTid)
        InterlockedCompareExchange(&g_ownerTid, tid, 0);
    return g_ownerTid == tid;
}

static FAA int WINAPI show_detour(BOOL show)
{
    InterlockedIncrement(&g_showCalls);
    if (!owner_thread()) {
        InterlockedIncrement(&g_showOther);
        return g_realShow(show);
    }
    if (!g_seeded) {
        int r = g_realShow(show);
        InterlockedIncrement(&g_showReal);
        g_shadow = r;
        g_seeded = true;
        return r;
    }
    int before = g_shadow;
    int after = show ? before + 1 : before - 1;
    if ((before >= 0) != (after >= 0)) {
        int r = g_realShow(show);
        InterlockedIncrement(&g_showReal);
        if ((r >= 0) != (after >= 0)) {
            // Someone else changed the real count: follow user32.
            InterlockedIncrement(&g_showResync);
            g_shadow = r;
            return r;
        }
    }
    g_shadow = after;
    if (after > g_showMaxShadow)
        g_showMaxShadow = after;
    return after;
}

static FAA HCURSOR WINAPI set_detour(HCURSOR h)
{
    InterlockedIncrement(&g_setCalls);
    if (!owner_thread()) {
        InterlockedIncrement(&g_setOther);
        return g_realSet(h);
    }
    int64_t now = qpc();
    if (g_haveCursor && h == g_lastCursor &&
        (g_refreshTicks <= 0 || now - g_lastSetReal < g_refreshTicks))
        return g_lastCursor;
    HCURSOR prev = g_realSet(h);
    InterlockedIncrement(&g_setReal);
    g_lastCursor = h;
    g_haveCursor = true;
    g_lastSetReal = now;
    return prev;
}

static FAA BOOL WINAPI clip_detour(const RECT *r)
{
    InterlockedIncrement(&g_clipCalls);
    if (!owner_thread()) {
        InterlockedIncrement(&g_clipOther);
        return g_realClip(r);
    }
    int64_t now = qpc();
    bool isNull = r == NULL;
    if (g_haveClip && isNull == g_lastClipNull &&
        (isNull || memcmp(r, &g_lastClip, sizeof(RECT)) == 0) &&
        (g_refreshTicks <= 0 || now - g_lastClipReal < g_refreshTicks))
        return TRUE;
    BOOL ok = g_realClip(r);
    InterlockedIncrement(&g_clipReal);
    if (ok) {
        g_haveClip = true;
        g_lastClipNull = isNull;
        if (!isNull)
            g_lastClip = *r;
        g_lastClipReal = now;
    } else {
        g_haveClip = false;
    }
    return ok;
}

int cursor_install(const DxhrfixOptions *o)
{
    int ms = o->cursor_refresh_ms;
    if (ms < 0) ms = 0;
    g_refreshTicks = g_qpcFreq * ms / 1000;
    int n = 0;
    n += iat_hook("USER32.dll", "ShowCursor", VA_IAT_SHOWCURSOR, (void *)show_detour,
                  (void **)&g_realShow);
    n += iat_hook("USER32.dll", "SetCursor", VA_IAT_SETCURSOR, (void *)set_detour,
                  (void **)&g_realSet);
    n += iat_hook("USER32.dll", "ClipCursor", VA_IAT_CLIPCURSOR, (void *)clip_detour,
                  (void **)&g_realClip);
    LOG(0, "cursor: %d of 3 IAT hooks installed (ShowCursor shadow count, SetCursor / ClipCursor "
           "dedupe, refresh %d ms)", n, ms);
    return n;
}

int cursor_format(char *buf, size_t n)
{
    if (!n) return 0;
    int k = _snprintf(buf, n - 1,
        "cursor: ShowCursor calls %ld real %ld (resync %ld, other thread %ld, max count %ld), "
        "SetCursor calls %ld real %ld (other %ld), ClipCursor calls %ld real %ld (other %ld)",
        (long)g_showCalls, (long)g_showReal, (long)g_showResync, (long)g_showOther,
        (long)g_showMaxShadow, (long)g_setCalls, (long)g_setReal, (long)g_setOther,
        (long)g_clipCalls, (long)g_clipReal, (long)g_clipOther);
    if (k < 0) k = (int)n - 1;
    buf[k] = 0;
    return k;
}
