/* hooklib: see hooklib.h for the threading assumption (install early, single thread). */
#include "hooklib.h"

#include <windows.h>
#include <string.h>

/* ---------------------------------------------------------------- decoder */

/* Size of ModRM (+SIB +disp) starting at p (p points at ModRM). 32-bit addressing only. */
static int modrm_len(const uint8_t* p)
{
    uint8_t m = p[0];
    int mod = m >> 6, rm = m & 7;
    int len = 1;
    if (mod == 3) return 1;
    if (rm == 4) {                       /* SIB */
        uint8_t sib = p[1];
        len += 1;
        if (mod == 0 && (sib & 7) == 5) len += 4;   /* [index*s + disp32] */
    } else if (mod == 0 && rm == 5) {
        len += 4;                        /* [disp32] absolute */
    }
    if (mod == 1) len += 1;
    else if (mod == 2) len += 4;
    return len;
}

extern "C" int hook_insn_len(const uint8_t* p, unsigned* flags, int* rel32_off)
{
    unsigned fl = 0;
    int pre = 0;
    int opsize16 = 0;
    if (rel32_off) *rel32_off = -1;

    /* prefixes: operand size and segment overrides (fs: for SEH prologues) */
    for (;;) {
        uint8_t b = p[pre];
        if (b == 0x66) { opsize16 = 1; pre++; }
        else if (b == 0x64 || b == 0x65 || b == 0x2E || b == 0x3E ||
                 b == 0x26 || b == 0x36) { pre++; }
        else break;
        if (pre > 4) return -1;
    }
    const uint8_t* q = p + pre;
    uint8_t op = q[0];
    int immz = opsize16 ? 2 : 4;         /* imm16/32 */
    int len = -1;

    if (op >= 0x50 && op <= 0x5F) len = 1;                 /* push/pop reg */
    else if (op >= 0x40 && op <= 0x4F) len = 1;            /* inc/dec reg */
    else if (op >= 0x91 && op <= 0x97) len = 1;            /* xchg eax,reg */
    else if (op >= 0xB0 && op <= 0xB7) len = 2;            /* mov r8,imm8 */
    else if (op >= 0xB8 && op <= 0xBF) len = 1 + immz;     /* mov reg,imm */
    else switch (op) {
    case 0x90: case 0xCC: case 0x9C: case 0x9D: case 0x60: case 0x61:
    case 0x99: case 0xFC: case 0xC9:                       /* nop int3 pushf popf pusha popa cdq cld leave */
        len = 1; break;
    case 0xC3:
        len = 1; fl |= HOOK_INSN_TERMINATOR; break;        /* ret */
    case 0xC2:
        len = 3; fl |= HOOK_INSN_TERMINATOR; break;        /* ret imm16 */
    case 0x6A: len = 2; break;                             /* push imm8 */
    case 0x68: len = 1 + immz; break;                      /* push imm32 */
    case 0xA1: case 0xA3: case 0xA0: case 0xA2:
        len = 5; break;                                    /* mov eax<->[moffs32] */
    /* ALU eax/al, imm */
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C:
    case 0x34: case 0x3C: case 0xA8:
        len = 2; break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D:
    case 0x35: case 0x3D: case 0xA9:
        len = 1 + immz; break;
    /* ALU r/m,reg and reg,r/m: add or adc sbb and sub xor cmp */
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3A: case 0x3B:
    case 0x84: case 0x85: case 0x86: case 0x87:            /* test, xchg */
    case 0x88: case 0x89: case 0x8A: case 0x8B:            /* mov */
    case 0x8D:                                             /* lea */
        len = 1 + modrm_len(q + 1); break;
    case 0x8F:                                             /* pop r/m */
        len = 1 + modrm_len(q + 1); break;
    case 0x80: case 0x83: case 0xC6: case 0xC0: case 0xC1:  /* grp imm8 */
        len = 1 + modrm_len(q + 1) + 1; break;
    case 0x81: case 0xC7: case 0x69:                       /* grp immz, imul immz */
        len = 1 + modrm_len(q + 1) + immz; break;
    case 0x6B:                                             /* imul imm8 */
        len = 1 + modrm_len(q + 1) + 1; break;
    case 0xD1: case 0xD3: case 0xD0: case 0xD2:            /* shifts by 1/cl */
        len = 1 + modrm_len(q + 1); break;
    case 0xF6: {
        int reg = (q[1] >> 3) & 7;
        len = 1 + modrm_len(q + 1) + (reg <= 1 ? 1 : 0); break;
    }
    case 0xF7: {
        int reg = (q[1] >> 3) & 7;
        len = 1 + modrm_len(q + 1) + (reg <= 1 ? immz : 0); break;
    }
    case 0xFF: {
        int reg = (q[1] >> 3) & 7;
        if (reg == 7) return -1;
        if (reg == 4 || reg == 5) fl |= HOOK_INSN_TERMINATOR;  /* jmp r/m */
        len = 1 + modrm_len(q + 1); break;
    }
    case 0xE8:
        len = 5; fl |= HOOK_INSN_REL32_CALL;
        if (rel32_off) *rel32_off = pre + 1;
        break;
    case 0xE9:
        len = 5; fl |= HOOK_INSN_REL32_JMP | HOOK_INSN_TERMINATOR;
        if (rel32_off) *rel32_off = pre + 1;
        break;
    case 0xEB:
        len = 2; fl |= HOOK_INSN_SHORT_BRANCH | HOOK_INSN_TERMINATOR; break;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        len = 2; fl |= HOOK_INSN_SHORT_BRANCH; break;
    case 0x0F: {
        uint8_t op2 = q[1];
        if (op2 >= 0x80 && op2 <= 0x8F) {
            len = 6; fl |= HOOK_INSN_REL32_JCC;
            if (rel32_off) *rel32_off = pre + 2;
        } else if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF ||
                   op2 == 0xAF || (op2 >= 0x40 && op2 <= 0x4F) ||
                   (op2 >= 0x90 && op2 <= 0x9F)) {
            len = 2 + modrm_len(q + 2);            /* movzx movsx imul cmovcc setcc */
        } else if (op2 == 0x1F) {
            len = 2 + modrm_len(q + 2);            /* multi-byte nop */
        } else {
            return -1;
        }
        break;
    }
    default:
        if (op >= 0x70 && op <= 0x7F) {
            len = 2; fl |= HOOK_INSN_SHORT_BRANCH; break;
        }
        return -1;
    }
    if (opsize16 && (fl & (HOOK_INSN_REL32_CALL | HOOK_INSN_REL32_JMP | HOOK_INSN_REL32_JCC)))
        return -1;                                 /* rel16 branches: refuse */
    if (flags) *flags = fl;
    return pre + len;
}

extern "C" int hook_calc_prologue(const void* target, size_t* out_len)
{
    if (!target || !out_len) return HOOK_ERR_NULL_ARG;
    const uint8_t* p = (const uint8_t*)target;
    size_t off = 0;
    while (off < 5) {
        unsigned fl = 0;
        int n = hook_insn_len(p + off, &fl, NULL);
        if (n <= 0) return HOOK_ERR_UNKNOWN_OPCODE;
        if (fl & HOOK_INSN_SHORT_BRANCH) return HOOK_ERR_SHORT_BRANCH;
        off += (size_t)n;
        if ((fl & HOOK_INSN_TERMINATOR) && off < 5) return HOOK_ERR_FUNC_TOO_SHORT;
        if (off > HOOK_MAX_PROLOGUE) return HOOK_ERR_PROLOGUE_TOO_LONG;
    }
    *out_len = off;
    return HOOK_OK;
}

/* ------------------------------------------------------- trampoline pool */

#define TRAMP_SLOT 48                     /* >= HOOK_MAX_PROLOGUE + 5, multiple of 16 */
#define POOL_PAGE 4096

static uint8_t* g_pool_page = NULL;
static size_t g_pool_used = 0;

/* Returns a slot; the containing page is left PAGE_EXECUTE_READWRITE (caller seals it). */
static uint8_t* tramp_alloc(void)
{
    if (!g_pool_page || g_pool_used + TRAMP_SLOT > POOL_PAGE) {
        g_pool_page = (uint8_t*)VirtualAlloc(NULL, POOL_PAGE, MEM_RESERVE | MEM_COMMIT,
                                             PAGE_EXECUTE_READWRITE);
        if (!g_pool_page) return NULL;
        memset(g_pool_page, 0xCC, POOL_PAGE);
        g_pool_used = 0;
    } else {
        DWORD old;
        if (!VirtualProtect(g_pool_page, POOL_PAGE, PAGE_EXECUTE_READWRITE, &old))
            return NULL;
    }
    uint8_t* s = g_pool_page + g_pool_used;
    g_pool_used += TRAMP_SLOT;
    return s;
}

static void tramp_seal(void)
{
    DWORD old;
    VirtualProtect(g_pool_page, POOL_PAGE, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), g_pool_page, POOL_PAGE);
}

/* ------------------------------------------------------------ hook table */

struct hook_rec {
    uint8_t* target;
    uint8_t* tramp;
    size_t len;
    uint8_t orig[HOOK_MAX_PROLOGUE];
};

static hook_rec g_hooks[HOOK_MAX_HOOKS];

static hook_rec* find_hook(const void* target)
{
    for (int i = 0; i < HOOK_MAX_HOOKS; i++)
        if (g_hooks[i].target == target) return &g_hooks[i];
    return NULL;
}

static void put_rel32(uint8_t* at, int32_t v) { memcpy(at, &v, 4); }
static int32_t get_rel32(const uint8_t* at) { int32_t v; memcpy(&v, at, 4); return v; }

static int write_code(void* dst, const void* src, size_t len)
{
    DWORD old;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old)) return HOOK_ERR_PROTECT;
    memcpy(dst, src, len);
    DWORD tmp;
    VirtualProtect(dst, len, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return HOOK_OK;
}

extern "C" int hook_install(void* target, const uint8_t* expect, size_t expect_len,
                            size_t prologue_len, void* detour, void** trampoline)
{
    if (!target || !expect || !expect_len || !detour) return HOOK_ERR_NULL_ARG;
    uint8_t* t = (uint8_t*)target;
    if (find_hook(t)) return HOOK_ERR_ALREADY_HOOKED;
    if (memcmp(t, expect, expect_len) != 0) return HOOK_ERR_BYTES_MISMATCH;

    size_t calc = 0;
    int st = hook_calc_prologue(t, &calc);
    if (st != HOOK_OK) return st;
    if (prologue_len == 0) prologue_len = calc;
    else if (prologue_len != calc) return HOOK_ERR_PROLOGUE_MISMATCH;
    if (expect_len < prologue_len) return HOOK_ERR_EXPECT_TOO_SHORT;

    /* Pre-check relocations before touching anything. */
    for (size_t off = 0; off < prologue_len;) {
        unsigned fl = 0; int ro = -1;
        int n = hook_insn_len(t + off, &fl, &ro);
        if (ro >= 0) {
            uint8_t* dest = t + off + n + get_rel32(t + off + ro);
            if (dest >= t && dest < t + prologue_len) return HOOK_ERR_UNRELOCATABLE;
        }
        off += (size_t)n;
    }

    hook_rec* rec = find_hook(NULL);
    if (!rec) return HOOK_ERR_TABLE_FULL;

    uint8_t* tr = tramp_alloc();
    if (!tr) return HOOK_ERR_ALLOC;

    /* Copy and relocate. */
    memcpy(tr, t, prologue_len);
    for (size_t off = 0; off < prologue_len;) {
        unsigned fl = 0; int ro = -1;
        int n = hook_insn_len(t + off, &fl, &ro);
        if (ro >= 0) {
            uint8_t* dest = t + off + n + get_rel32(t + off + ro);
            put_rel32(tr + off + ro, (int32_t)(dest - (tr + off + n)));
        }
        off += (size_t)n;
    }
    /* jmp back to target + prologue_len */
    tr[prologue_len] = 0xE9;
    put_rel32(tr + prologue_len + 1, (int32_t)((t + prologue_len) - (tr + prologue_len + 5)));
    tramp_seal();

    /* Patch target: jmp rel32 detour, pad with int3. */
    uint8_t patch[HOOK_MAX_PROLOGUE];
    memset(patch, 0xCC, sizeof patch);
    patch[0] = 0xE9;
    put_rel32(patch + 1, (int32_t)((uint8_t*)detour - (t + 5)));

    rec->len = prologue_len;
    memcpy(rec->orig, t, prologue_len);
    if (trampoline) *trampoline = tr;   /* publish before the jmp goes live */
    st = write_code(t, patch, prologue_len);
    if (st != HOOK_OK) {
        if (trampoline) *trampoline = NULL;
        return st;                       /* slot leaked, target untouched */
    }
    rec->target = t;
    rec->tramp = tr;
    return HOOK_OK;
}

extern "C" int hook_remove(void* target)
{
    if (!target) return HOOK_ERR_NULL_ARG;
    hook_rec* rec = find_hook(target);
    if (!rec) return HOOK_ERR_NOT_HOOKED;
    if (rec->target[0] != 0xE9) return HOOK_ERR_MODIFIED;
    int st = write_code(rec->target, rec->orig, rec->len);
    if (st != HOOK_OK) return st;
    rec->target = NULL;                  /* trampoline slot intentionally not reused */
    return HOOK_OK;
}

extern "C" int patch_bytes(void* target, const uint8_t* expect, const uint8_t* replacement,
                           size_t len)
{
    if (!target || !expect || !replacement || !len) return HOOK_ERR_NULL_ARG;
    if (memcmp(target, expect, len) != 0) return HOOK_ERR_BYTES_MISMATCH;
    return write_code(target, replacement, len);
}

extern "C" const char* hook_strerror(int s)
{
    switch (s) {
    case HOOK_OK: return "ok";
    case HOOK_ERR_NULL_ARG: return "null argument or zero length";
    case HOOK_ERR_BYTES_MISMATCH: return "bytes at target do not match expected";
    case HOOK_ERR_EXPECT_TOO_SHORT: return "expect_len shorter than prologue";
    case HOOK_ERR_UNKNOWN_OPCODE: return "unknown opcode in prologue";
    case HOOK_ERR_SHORT_BRANCH: return "short relative branch in prologue";
    case HOOK_ERR_UNRELOCATABLE: return "unrelocatable instruction in prologue";
    case HOOK_ERR_PROLOGUE_MISMATCH: return "prologue_len differs from decoded boundary";
    case HOOK_ERR_PROLOGUE_TOO_LONG: return "prologue too long";
    case HOOK_ERR_FUNC_TOO_SHORT: return "function ends before 5 bytes";
    case HOOK_ERR_PROTECT: return "VirtualProtect failed";
    case HOOK_ERR_ALLOC: return "trampoline allocation failed";
    case HOOK_ERR_ALREADY_HOOKED: return "target already hooked";
    case HOOK_ERR_NOT_HOOKED: return "target not hooked";
    case HOOK_ERR_TABLE_FULL: return "hook table full";
    case HOOK_ERR_MODIFIED: return "hook jmp was overwritten by someone else";
    default: return "unknown status";
    }
}
