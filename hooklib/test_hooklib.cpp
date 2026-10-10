/*
 * Unit tests for hooklib, built as a 32-bit Windows console exe and run under
 * wine. Targets are written in assembly so their prologues are exact and
 * cover the cases the library must handle: a normal frame prologue, a
 * call rel32 inside the first 5 bytes, and a stdcall callee (ret imm16).
 */
#include "hooklib.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

/* ---- assembly targets (AT&T, 32-bit) ---- */
extern "C" int helper42(void);
extern "C" int tgt_add(int a, int b);                      /* cdecl */
extern "C" int tgt_call(void);                             /* cdecl, call rel32 first */
extern "C" int __attribute__((stdcall)) tgt_stdcall(int a, int b);

asm(
    ".text\n"

    ".globl _helper42\n"
    "_helper42:\n"
    "  movl $35, %eax\n"
    "  ret\n"

    /* 55 | 89 E5 | 8B 45 08 ...  -> prologue boundary >=5 is 6 */
    ".globl _tgt_add\n"
    "_tgt_add:\n"
    "  push %ebp\n"
    "  mov %esp, %ebp\n"
    "  mov 8(%ebp), %eax\n"
    "  add 12(%ebp), %eax\n"
    "  add $100, %eax\n"
    "  pop %ebp\n"
    "  ret\n"

    /* E8 rel32 (call) is exactly the first 5 bytes */
    ".globl _tgt_call\n"
    "_tgt_call:\n"
    "  call _helper42\n"
    "  add $7, %eax\n"
    "  ret\n"

    /* stdcall: returns a*b+1, cleans 8 bytes (C2 08 00) */
    ".globl _tgt_stdcall@8\n"
    "_tgt_stdcall@8:\n"
    "  push %ebp\n"
    "  mov %esp, %ebp\n"
    "  mov 8(%ebp), %eax\n"
    "  imul 12(%ebp), %eax\n"
    "  add $1, %eax\n"
    "  pop %ebp\n"
    "  ret $8\n"
);

/* ---- detours ---- */
typedef int (*add_fn)(int, int);
typedef int (*call_fn)(void);
typedef int (__attribute__((stdcall)) *stdcall_fn)(int, int);

static add_fn      tr_add;
static call_fn     tr_call;
static stdcall_fn  tr_stdcall;
static int flag_add, flag_call, flag_stdcall;

static int det_add(int a, int b) {
    flag_add = 1;
    return tr_add(a, b) + 1000;            /* original + marker */
}
static int det_call(void) {
    flag_call = 1;
    return tr_call() + 1000;
}
static int __attribute__((stdcall)) det_stdcall(int a, int b) {
    flag_stdcall = 1;
    return tr_stdcall(a, b) + 1000;
}

/* ---- tiny test framework ---- */
static int fails = 0, checks = 0;
#define CHECK(cond, msg) do { checks++; \
    if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } \
    else printf("  ok:   %s\n", msg); } while (0)

int main(void)
{
    printf("hooklib tests\n");

    /* decoder / prologue sizing */
    size_t pl = 0;
    CHECK(hook_calc_prologue((void*)tgt_add, &pl) == HOOK_OK && pl == 6,
          "calc_prologue tgt_add == 6");
    CHECK(hook_calc_prologue((void*)tgt_call, &pl) == HOOK_OK && pl == 5,
          "calc_prologue tgt_call == 5 (call rel32)");
    CHECK(hook_calc_prologue((void*)tgt_stdcall, &pl) == HOOK_OK && pl == 6,
          "calc_prologue tgt_stdcall == 6");

    /* baseline */
    CHECK(tgt_add(2, 3) == 105, "tgt_add baseline 105");
    CHECK(tgt_call() == 42, "tgt_call baseline 42");
    CHECK(tgt_stdcall(6, 7) == 43, "tgt_stdcall baseline 43");

    /* install hooks (expect bytes = current prologue) */
    uint8_t ex[HOOK_MAX_PROLOGUE];

    memcpy(ex, (void*)tgt_add, 6);
    CHECK(hook_install((void*)tgt_add, ex, 6, 6, (void*)det_add, (void**)&tr_add) == HOOK_OK,
          "install tgt_add");
    memcpy(ex, (void*)tgt_call, 5);
    CHECK(hook_install((void*)tgt_call, ex, 5, 0, (void*)det_call, (void**)&tr_call) == HOOK_OK,
          "install tgt_call (auto prologue, relocate call rel32)");
    memcpy(ex, (void*)tgt_stdcall, 6);
    CHECK(hook_install((void*)tgt_stdcall, ex, 6, 0, (void*)det_stdcall, (void**)&tr_stdcall) == HOOK_OK,
          "install tgt_stdcall (auto prologue == 6)");

    /* detour ran AND trampoline returned the original result */
    CHECK(tgt_add(2, 3) == 1105 && flag_add, "tgt_add hooked -> detour + original(105)");
    CHECK(tgt_call() == 1042 && flag_call, "tgt_call hooked -> detour + original(42)");
    CHECK(tgt_stdcall(6, 7) == 1043 && flag_stdcall, "tgt_stdcall hooked -> detour + original(43)");

    /* trampolines reproduce the original directly */
    CHECK(tr_add(10, 20) == 130, "trampoline tgt_add direct == 130");
    CHECK(tr_call() == 42, "trampoline tgt_call direct == 42");
    CHECK(tr_stdcall(3, 4) == 13, "trampoline tgt_stdcall direct == 13");

    /* double install refused */
    memcpy(ex, (void*)tgt_add, 6);           /* bytes are now the jmp, but target check comes first */
    CHECK(hook_install((void*)tgt_add, ex, 6, 6, (void*)det_add, (void**)&tr_add)
          == HOOK_ERR_ALREADY_HOOKED, "double install refused");

    /* remove restores original behaviour */
    CHECK(hook_remove((void*)tgt_add) == HOOK_OK, "remove tgt_add");
    CHECK(hook_remove((void*)tgt_call) == HOOK_OK, "remove tgt_call");
    CHECK(hook_remove((void*)tgt_stdcall) == HOOK_OK, "remove tgt_stdcall");
    CHECK(tgt_add(2, 3) == 105, "tgt_add restored");
    CHECK(tgt_call() == 42, "tgt_call restored");
    CHECK(tgt_stdcall(6, 7) == 43, "tgt_stdcall restored");
    CHECK(hook_remove((void*)tgt_add) == HOOK_ERR_NOT_HOOKED, "remove unknown refused");

    /* patch_bytes: push 1 -> push 2 (immediate swap) on a scratch buffer */
    {
        DWORD old;
        static uint8_t scratch[8] = { 0x6A, 0x01, 0xC3 };   /* push 1 ; ret */
        VirtualProtect(scratch, sizeof scratch, PAGE_EXECUTE_READWRITE, &old);
        uint8_t want[2] = { 0x6A, 0x01 }, repl[2] = { 0x6A, 0x02 };
        CHECK(patch_bytes(scratch, want, repl, 2) == HOOK_OK, "patch_bytes push1->push2");
        CHECK(scratch[1] == 0x02, "patch_bytes wrote new immediate");
        uint8_t bad[2] = { 0x6A, 0x09 };
        CHECK(patch_bytes(scratch, bad, repl, 2) == HOOK_ERR_BYTES_MISMATCH,
              "patch_bytes mismatch refused");
    }

    /* byte-mismatch refusal on install (wrong expected bytes) */
    {
        uint8_t wrong[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
        void* tr = NULL;
        CHECK(hook_install((void*)tgt_add, wrong, 6, 6, (void*)det_add, &tr)
              == HOOK_ERR_BYTES_MISMATCH, "install byte-mismatch refused");
        CHECK(tgt_add(1, 1) == 102, "tgt_add untouched after refused install");
    }

    /* null-arg refusal */
    CHECK(hook_install(NULL, (const uint8_t*)"x", 1, 0, (void*)det_add, NULL)
          == HOOK_ERR_NULL_ARG, "null target refused");

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
