/*
 * hooklib: dependency-free x86-32 inline hook library (MinGW / MSVC ABI).
 *
 * THREADING ASSUMPTION
 *   hook_install, hook_remove and patch_bytes are NOT safe against other
 *   threads executing the bytes being rewritten. They are meant to be called
 *   once, early (DllMain PROCESS_ATTACH or first call into the proxy), before
 *   the game creates its render / worker threads. No threads are suspended
 *   and no instruction pointers are fixed up. The internal hook table is not
 *   locked either; call the API from a single thread.
 *
 * TRAMPOLINES
 *   Trampolines live in a private pool (VirtualAlloc pages). Pages are
 *   PAGE_EXECUTE_READ except while a new trampoline is written (briefly
 *   PAGE_EXECUTE_READWRITE, so already running trampolines on the same page
 *   keep working). Trampoline slots are never freed or reused, so a thread
 *   still inside a trampoline after hook_remove stays valid.
 */
#ifndef HOOKLIB_H
#define HOOKLIB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum hook_status {
    HOOK_OK = 0,
    HOOK_ERR_NULL_ARG = 1,            /* null pointer or zero length */
    HOOK_ERR_BYTES_MISMATCH = 2,      /* bytes at target != expect */
    HOOK_ERR_EXPECT_TOO_SHORT = 3,    /* expect_len < prologue_len */
    HOOK_ERR_UNKNOWN_OPCODE = 4,      /* decoder cannot size an instruction */
    HOOK_ERR_SHORT_BRANCH = 5,        /* jcc/jmp/loop rel8 inside prologue */
    HOOK_ERR_UNRELOCATABLE = 6,       /* rel32 branch into the prologue itself, etc */
    HOOK_ERR_PROLOGUE_MISMATCH = 7,   /* caller prologue_len != decoded minimum */
    HOOK_ERR_PROLOGUE_TOO_LONG = 8,   /* > HOOK_MAX_PROLOGUE */
    HOOK_ERR_FUNC_TOO_SHORT = 9,      /* ret / jmp ends the function before 5 bytes */
    HOOK_ERR_PROTECT = 10,            /* VirtualProtect failed */
    HOOK_ERR_ALLOC = 11,              /* VirtualAlloc failed / pool full */
    HOOK_ERR_ALREADY_HOOKED = 12,     /* target already hooked by hooklib */
    HOOK_ERR_NOT_HOOKED = 13,         /* hook_remove on unknown target */
    HOOK_ERR_TABLE_FULL = 14,         /* more than HOOK_MAX_HOOKS hooks */
    HOOK_ERR_MODIFIED = 15            /* hook_remove: our jmp was overwritten */
};

#define HOOK_MAX_PROLOGUE 24
#define HOOK_MAX_HOOKS 64

/* Decoder flags (out param of hook_insn_len). */
#define HOOK_INSN_REL32_CALL   0x01  /* E8 rel32 */
#define HOOK_INSN_REL32_JMP    0x02  /* E9 rel32 */
#define HOOK_INSN_REL32_JCC    0x04  /* 0F 80..8F rel32 */
#define HOOK_INSN_SHORT_BRANCH 0x08  /* EB, 70..7F, E0..E3: not relocatable */
#define HOOK_INSN_TERMINATOR   0x10  /* ret, ret imm16, jmp rel32/rel8, jmp r/m */

/*
 * Length of the instruction at p (common prologue subset), or -1 if unknown.
 * flags (may be NULL) receives HOOK_INSN_* bits. rel32_off (may be NULL)
 * receives the offset of the rel32 field within the instruction, if any.
 */
int hook_insn_len(const uint8_t* p, unsigned* flags, int* rel32_off);

/*
 * Smallest instruction boundary >= 5 starting at target. Returns HOOK_OK and
 * writes *out_len, or an error (UNKNOWN_OPCODE, SHORT_BRANCH, FUNC_TOO_SHORT,
 * PROLOGUE_TOO_LONG).
 */
int hook_calc_prologue(const void* target, size_t* out_len);

/*
 * Install an inline hook.
 *   expect/expect_len: bytes that must be present at target (expect_len must
 *     be >= prologue_len so no unverified byte is ever copied).
 *   prologue_len: number of original bytes to relocate; 0 = use the decoded
 *     minimum. A nonzero value must equal the decoded minimum.
 *   detour: function that receives control (same calling convention).
 *   trampoline (may be NULL): receives a pointer that runs the original.
 * On failure target is left untouched.
 */
int hook_install(void* target, const uint8_t* expect, size_t expect_len,
                 size_t prologue_len, void* detour, void** trampoline);

/* Restore the original bytes. The trampoline stays valid (leaked by design). */
int hook_remove(void* target);

/* Verify expect then overwrite len bytes with replacement (e.g. push 1 -> push 2). */
int patch_bytes(void* target, const uint8_t* expect, const uint8_t* replacement,
                size_t len);

const char* hook_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
