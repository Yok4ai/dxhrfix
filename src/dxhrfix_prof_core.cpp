// Pure helpers of the hitch profiler. See dxhrfix_prof_core.h.
#include "dxhrfix_prof_core.h"

#include <stdlib.h>
#include <string.h>

bool prof_is_call_site(const uint8_t *ra)
{
    if (ra[-5] == 0xE8)                                   // call rel32
        return true;
    if (ra[-6] == 0xFF && ra[-5] == 0x15)                 // call [disp32]
        return true;
    if (ra[-2] == 0xFF) {
        uint8_t m = ra[-1];
        if ((m & 0xF8) == 0xD0)                           // call reg
            return true;
        if ((m & 0xF8) == 0x10 && m != 0x14 && m != 0x15) // call [reg]
            return true;
    }
    if (ra[-3] == 0xFF) {
        uint8_t m = ra[-2];
        if ((m & 0xF8) == 0x50 && m != 0x54)              // call [reg+d8]
            return true;
        if (m == 0x14)                                    // call [sib]
            return true;
    }
    if (ra[-4] == 0xFF && ra[-3] == 0x54)                 // call [sib+d8]
        return true;
    if (ra[-6] == 0xFF && (ra[-5] & 0xF8) == 0x90 && ra[-5] != 0x94) // call [reg+d32]
        return true;
    if (ra[-7] == 0xFF && ra[-6] == 0x94)                 // call [sib+d32]
        return true;
    return false;
}

int prof_find_range(const CodeRange *r, int n, uint32_t va)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (va < r[mid].lo)
            hi = mid - 1;
        else if (va >= r[mid].hi)
            lo = mid + 1;
        else
            return mid;
    }
    return -1;
}

int prof_scan_stack(const uint32_t *sp, size_t nwords, const CodeRange *r, int nr,
                    uint32_t *out, int max)
{
    if (nr <= 0 || max <= 0)
        return 0;
    uint32_t minLo = r[0].lo, maxHi = r[nr - 1].hi;
    int k = 0;
    for (size_t i = 0; i < nwords && k < max; i++) {
        uint32_t w = sp[i];
        if (w < minLo || w >= maxHi)
            continue;
        int ri = prof_find_range(r, nr, w);
        if (ri < 0 || w - r[ri].lo < 7)
            continue;
        if (k && out[k - 1] == w)
            continue;   // stale copy right next to the same address: one frame
        if (prof_is_call_site((const uint8_t *)(uintptr_t)w))
            out[k++] = w;
    }
    return k;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int sym_cmp(const void *a, const void *b)
{
    uint32_t x = ((const Sym *)a)->va, y = ((const Sym *)b)->va;
    return x < y ? -1 : x > y;
}

int symtab_parse(SymTable *t, char *buf, size_t len)
{
    t->syms = NULL;
    t->n = 0;
    size_t lines = 1;
    for (size_t i = 0; i < len; i++)
        lines += buf[i] == '\n';
    t->syms = (Sym *)malloc(lines * sizeof(Sym));
    if (!t->syms)
        return 0;
    int n = 0;
    bool sorted = true;
    size_t i = 0;
    while (i < len) {
        size_t s = i;
        while (i < len && buf[i] != '\n')
            i++;
        size_t e = i;            // line is buf[s, e)
        i++;
        if (e > s && buf[e - 1] == '\r')
            e--;
        if (e <= s || buf[s] == '#')
            continue;
        size_t p = s;
        if (e - p > 2 && buf[p] == '0' && (buf[p + 1] == 'x' || buf[p + 1] == 'X'))
            p += 2;
        uint32_t va = 0;
        int digits = 0;
        while (p < e && hexval(buf[p]) >= 0 && digits < 9) {
            va = (va << 4) | (uint32_t)hexval(buf[p]);
            p++;
            digits++;
        }
        if (!digits || digits > 8 || p >= e || buf[p] != '\t')
            continue;
        p++;
        size_t ns = p;
        while (p < e && buf[p] != '\t')
            p++;
        size_t ne = p;
        if (ne == ns)
            continue;
        uint32_t size = 0;
        if (p < e) {
            p++;
            while (p < e && buf[p] >= '0' && buf[p] <= '9')
                size = size * 10 + (uint32_t)(buf[p++] - '0');
        }
        buf[ne] = 0;
        if (n && va < t->syms[n - 1].va)
            sorted = false;
        t->syms[n].va = va;
        t->syms[n].size = size;
        t->syms[n].name = buf + ns;
        n++;
    }
    if (!sorted)
        qsort(t->syms, n, sizeof(Sym), sym_cmp);
    t->n = n;
    return n;
}

void symtab_free(SymTable *t)
{
    free(t->syms);
    t->syms = NULL;
    t->n = 0;
}

const Sym *symtab_find(const SymTable *t, uint32_t va, uint32_t *off)
{
    int lo = 0, hi = t->n - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (t->syms[mid].va <= va) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0)
        return NULL;
    if (off)
        *off = va - t->syms[best].va;
    return &t->syms[best];
}

uint32_t prof_hash(const uint32_t *p, int n, uint32_t h)
{
    for (int i = 0; i < n; i++) {
        uint32_t v = p[i];
        for (int b = 0; b < 4; b++) {
            h ^= (v >> (b * 8)) & 0xFF;
            h *= 16777619u;
        }
    }
    return h;
}
