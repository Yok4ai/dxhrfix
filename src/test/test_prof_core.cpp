// Unit test for the hitch profiler helpers (symbol table, call site
// heuristic, stack scan), run under host wine: make -C src/test test
#include "../dxhrfix_prof_core.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_symtab(void)
{
    char buf[] =
        "# comment line\r\n"
        "00401000\tFUN_00401000\t15\n"
        "0053a170\trenderFrame\t1200\r\n"
        "bogus line\n"
        "0054c9f0\tpresent\n"
        "00401010\tFUN_00401010\t35\n"          // out of order: forces the sort
        "zz\tbad\t1\n"
        "123456789\ttoolong\t1\n"
        "\n"
        "0x00a39574\t_threadstartex\t140";      // no trailing newline, 0x prefix
    SymTable t;
    int n = symtab_parse(&t, buf, strlen(buf));
    CHECK(n == 5);
    CHECK(t.n == 5);
    for (int i = 1; i < t.n; i++)
        CHECK(t.syms[i - 1].va < t.syms[i].va);
    uint32_t off = 99;
    const Sym *s = symtab_find(&t, 0x53A170, &off);
    CHECK(s && strcmp(s->name, "renderFrame") == 0 && off == 0 && s->size == 1200);
    s = symtab_find(&t, 0x53A584, &off);
    CHECK(s && strcmp(s->name, "renderFrame") == 0 && off == 0x414);
    s = symtab_find(&t, 0x54C9F5, &off);
    CHECK(s && strcmp(s->name, "present") == 0 && off == 5 && s->size == 0);
    s = symtab_find(&t, 0x401012, &off);
    CHECK(s && strcmp(s->name, "FUN_00401010") == 0 && off == 2);
    s = symtab_find(&t, 0x400FFF, &off);
    CHECK(s == NULL);
    s = symtab_find(&t, 0xA39600, &off);
    CHECK(s && strcmp(s->name, "_threadstartex") == 0 && off == 0x8C);
    symtab_free(&t);

    SymTable e;
    char empty[] = "";
    CHECK(symtab_parse(&e, empty, 0) == 0);
    CHECK(symtab_find(&e, 0x401000, &off) == NULL);
    symtab_free(&e);
}

// Synthetic code block: each return address follows one call form.
static uint8_t code[256];
static uint32_t ra_of[16];
static int nra;

static void emit(int at, const uint8_t *insn, int len)
{
    memcpy(code + at, insn, len);
    ra_of[nra++] = (uint32_t)(uintptr_t)(code + at + len);
}

static void test_call_sites(void)
{
    memset(code, 0x90, sizeof(code));
    static const uint8_t e8[] = {0xE8, 0x11, 0x22, 0x33, 0x44};        // call rel32
    static const uint8_t ff15[] = {0xFF, 0x15, 0xA0, 0x12, 0xA8, 0x00}; // call [0xa812a0]
    static const uint8_t ffd2[] = {0xFF, 0xD2};                          // call edx
    static const uint8_t ff10[] = {0xFF, 0x10};                          // call [eax]
    static const uint8_t ff5004[] = {0xFF, 0x50, 0x04};                  // call [eax+4]
    static const uint8_t ff9x[] = {0xFF, 0x92, 0x00, 0x01, 0x00, 0x00};  // call [edx+0x100]
    static const uint8_t ff14[] = {0xFF, 0x14, 0x85};                    // call [eax*4] (sib)
    static const uint8_t ff54[] = {0xFF, 0x54, 0x24, 0x08};              // call [esp+8]
    static const uint8_t ff94[] = {0xFF, 0x94, 0x24, 0x00, 0x01, 0x00, 0x00}; // call [esp+0x100]
    nra = 0;
    emit(16, e8, sizeof(e8));
    emit(40, ff15, sizeof(ff15));
    emit(64, ffd2, sizeof(ffd2));
    emit(88, ff10, sizeof(ff10));
    emit(112, ff5004, sizeof(ff5004));
    emit(136, ff9x, sizeof(ff9x));
    emit(160, ff14, sizeof(ff14));
    emit(184, ff54, sizeof(ff54));
    emit(208, ff94, sizeof(ff94));
    for (int i = 0; i < nra; i++)
        CHECK(prof_is_call_site((const uint8_t *)(uintptr_t)ra_of[i]));
    // Not call sites: plain nops, a jmp rel32, push imm32.
    CHECK(!prof_is_call_site(code + 12));
    static const uint8_t e9[] = {0xE9, 0x00, 0x00, 0x00, 0x00};
    memcpy(code + 230, e9, 5);
    CHECK(!prof_is_call_site(code + 235));
    static const uint8_t ff25[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}; // jmp [disp32]
    memcpy(code + 240, ff25, 6);
    CHECK(!prof_is_call_site(code + 246));
}

static void test_scan(void)
{
    CodeRange r[3];
    // Range 0: a fake range that no stack value points into (never read).
    // Range 1: the code block. Range 2: only used for the lookup checks.
    uint32_t cb = (uint32_t)(uintptr_t)code;
    r[0].lo = 0x1000; r[0].hi = 0x2000;
    r[1].lo = cb; r[1].hi = cb + sizeof(code);
    r[2].lo = 0xF0000000u; r[2].hi = 0xF0001000u;
    CHECK(prof_find_range(r, 3, cb + 5) == 1);
    CHECK(prof_find_range(r, 3, 0x1FFF) == 0);
    CHECK(prof_find_range(r, 3, 0x2000) == -1);
    CHECK(prof_find_range(r, 3, cb + sizeof(code)) == -1);

    uint32_t stack[64];
    for (int i = 0; i < 64; i++)
        stack[i] = 0x12345678u + i;          // junk outside every range
    stack[0] = cb + 12;                       // in range, not after a call
    stack[2] = ra_of[0];                      // e8
    stack[3] = cb + 3;                        // in range, < 7 bytes past lo
    stack[5] = ra_of[1];                      // ff15
    stack[9] = 0xF0000800u;                   // outside the 2 ranges passed to the scan
    stack[12] = ra_of[4];                     // ff 50 04
    stack[20] = ra_of[8];                     // ff 94 sib d32
    stack[21] = ra_of[2];
    stack[22] = ra_of[2];                     // adjacent repeat of the same frame: dropped
    stack[23] = 0x12345678u;                  // junk between two copies
    stack[24] = ra_of[2];                     // still the last accepted: dropped too
    stack[25] = ra_of[8];                     // a different address again: kept
    stack[40] = ra_of[3];                     // beyond the scanned length below
    // Only ranges 0 and 1 are passed, so 0xF0000800 is filtered before any byte read.
    uint32_t out[24];
    int n = prof_scan_stack(stack, 32, r, 2, out, 24);
    CHECK(n == 6);
    if (n == 6) {
        CHECK(out[0] == ra_of[0]);
        CHECK(out[1] == ra_of[1]);
        CHECK(out[2] == ra_of[4]);
        CHECK(out[3] == ra_of[8]);
        CHECK(out[4] == ra_of[2]);
        CHECK(out[5] == ra_of[8]);
    }
    // max caps the output
    n = prof_scan_stack(stack, 32, r, 2, out, 2);
    CHECK(n == 2 && out[0] == ra_of[0] && out[1] == ra_of[1]);
    // nothing to scan
    CHECK(prof_scan_stack(stack, 0, r, 2, out, 24) == 0);
    CHECK(prof_scan_stack(stack, 32, r, 0, out, 24) == 0);
    // full length finds the deeper one as well
    n = prof_scan_stack(stack, 64, r, 2, out, 24);
    CHECK(n == 7 && out[6] == ra_of[3]);
}

static void test_hash(void)
{
    uint32_t a[3] = {1, 2, 3}, b[3] = {1, 2, 4};
    CHECK(prof_hash(a, 3, 2166136261u) == prof_hash(a, 3, 2166136261u));
    CHECK(prof_hash(a, 3, 2166136261u) != prof_hash(b, 3, 2166136261u));
    CHECK(prof_hash(a, 0, 7) == 7);
}

int main(void)
{
    test_symtab();
    test_call_sites();
    test_scan();
    test_hash();
    printf("%s (%d failures)\n", fails ? "TEST FAILED" : "TEST OK", fails);
    return fails ? 1 : 0;
}
