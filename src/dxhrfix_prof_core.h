/* Pure helpers of the hitch profiler (src/dxhrfix_profiler.cpp): symbol
 * table, call site heuristic, conservative stack scan. No Win32 calls, no
 * allocation except in symtab_parse, so they can run on a thread that has
 * other threads suspended (scan, call site test) and be unit tested on
 * synthetic buffers (src/test/test_prof_core.cpp). */
#ifndef DXHRFIX_PROF_CORE_H
#define DXHRFIX_PROF_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Executable address range [lo, hi), sorted by lo, non overlapping. */
struct CodeRange {
    uint32_t lo, hi;
};

/* True if the bytes before ra end with a call instruction: E8 rel32,
 * FF 15 disp32, or FF /2 with a register, [reg], [reg+d8], [reg+d32] or SIB
 * operand. Reads ra[-7..-1]; the caller guarantees they are readable. */
bool prof_is_call_site(const uint8_t *ra);

/* Binary search: index of the range holding va, or -1. */
int prof_find_range(const CodeRange *r, int n, uint32_t va);

/* Conservative scan of nwords stack dwords at sp. A dword counts as a
 * return address if it lies inside a range at least 7 bytes past its start
 * and prof_is_call_site holds. A repeat of the previously accepted address is
 * dropped (stale copies of one frame). Writes up to max of them to out,
 * innermost first, and returns the count. Does not allocate or call the OS. */
int prof_scan_stack(const uint32_t *sp, size_t nwords, const CodeRange *r, int nr,
                    uint32_t *out, int max);

struct Sym {
    uint32_t va;     /* preferred base VA */
    uint32_t size;   /* 0 = unknown */
    const char *name;
};

struct SymTable {
    Sym *syms;
    int n;
};

/* Parse "hexVA<TAB>name[<TAB>size]" lines in place (buf is modified and
 * must outlive the table; '#' lines and malformed lines are skipped).
 * Allocates t->syms with malloc and sorts it. Returns the count. */
int symtab_parse(SymTable *t, char *buf, size_t len);
void symtab_free(SymTable *t);

/* Symbol with the largest va <= va, or NULL. *off receives va - sym.va. */
const Sym *symtab_find(const SymTable *t, uint32_t va, uint32_t *off);

/* 32-bit FNV-1a over n dwords. */
uint32_t prof_hash(const uint32_t *p, int n, uint32_t h);

#endif
