/*
 * engine.h: DXHRDC.exe (Deus Ex HR Director's Cut, Steam 238010) engine layouts
 * for the DFEngine.dll proxy hooks. 32-bit x86 only. C99 and C++ compatible.
 *
 * Source: stock exe sha256 8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1
 * (image base 0x400000, DYNAMIC_BASE set). All addresses below are preferred VAs;
 * at runtime use GetModuleHandleA(NULL) + RVA, RVA = VA - 0x400000.
 * .text file offset = VA - 0x400C00. See SPEC.md for semantics and evidence.
 *
 * Confidence tags in comments:
 *   [V] verified from disassembly in this exe
 *   [R] matches rrika/cdcEngineDXHR reconstruction or RTTI, consistent with disasm
 *   [?] uncertain, inferred
 */
#ifndef DXHR_ENGINE_H
#define DXHR_ENGINE_H

#include <stdint.h>
#include <stddef.h>

#if !defined(_M_IX86) && !defined(__i386__)
#error "engine.h describes a 32-bit x86 process"
#endif

#if defined(__GNUC__)
#define DX_THISCALL __attribute__((thiscall))
#define DX_STDCALL  __attribute__((stdcall))
#define DX_FASTCALL __attribute__((fastcall))
#else
#define DX_THISCALL __thiscall
#define DX_STDCALL  __stdcall
#define DX_FASTCALL __fastcall
#endif

#if defined(__cplusplus)
#define DX_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define DX_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
#define DX_CHECK_OFF(T, f, o) DX_STATIC_ASSERT(offsetof(T, f) == (o), #T "." #f " offset")
#define DX_CHECK_SIZE(T, s)   DX_STATIC_ASSERT(sizeof(T) == (s), "sizeof " #T)

/* ------------------------------------------------------------------------ */
/* Image                                                                     */
/* ------------------------------------------------------------------------ */
#define DX_IMAGE_BASE        0x00400000u
#define DX_RVA(va)           ((uint32_t)(va) - DX_IMAGE_BASE)
#define DX_TEXT_FILEOFF(va)  ((uint32_t)(va) - 0x00400C00u)
#define DX_STOCK_SHA256      "8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1"

/* ------------------------------------------------------------------------ */
/* Globals (VA). Read as *(T*)(base + DX_RVA(x)).                            */
/* ------------------------------------------------------------------------ */
#define DX_G_DEVICE_MANAGER   0x012AB940u /* PCDX11DeviceManager* [V] set once at 0x54C415   */
#define DX_G_RENDER_DEVICE10  0x00EAAB48u /* PCDX11RenderDevice* + 0x10 (CommonRenderDevice part) [V] */
#define DX_G_ALLOC_HOLDER     0x019ED7B0u /* +0x48: allocator object; vtbl +0x34 alloc(size,flags), +0x38 free(p) [V] */
#define DX_G_SINGLE_THREADED  0x00EAAB78u /* uint8, BSS, never written by direct ref: always 0 [V] */
#define DX_G_PRECREATE_FLAG   0x012AB963u /* uint8 set 1/0 around FUN_0054C8D0 request, never read [V] */
#define DX_G_DFMANAGER        0x01C78C84u /* DxDfManager singleton (returned by FUN_00628AA0) [V] */

/* IAT slots (VA) used by the engine code around the hooks [V] */
#define DX_IAT_GetDFEngine          0x00A81040u
#define DX_IAT_ReleaseSemaphore     0x00A810DCu
#define DX_IAT_InterlockedIncrement 0x00A81104u
#define DX_IAT_EnterCriticalSection 0x00A8121Cu
#define DX_IAT_QueryPerfFrequency   0x00A81260u
#define DX_IAT_QueryPerfCounter     0x00A81264u
#define DX_IAT_CreateThread         0x00A81270u
#define DX_IAT_WaitForSingleObject  0x00A81284u
#define DX_IAT_LeaveCriticalSection 0x00A81294u
#define DX_IAT_WaitForMultipleObj   0x00A81298u
#define DX_IAT_Sleep                0x00A812A0u

/* ------------------------------------------------------------------------ */
/* Hook sites (VA). _CC = calling convention, _ORIG = original prologue.    */
/* "jmp5_len" = smallest whole instruction span >= 5 bytes.                 */
/* ------------------------------------------------------------------------ */

/* PCDX11AsyncCreateResource::request(uint32 arg). thiscall ecx=item, 1 stack arg, ret 4. [V]
 * Body is 0x22 bytes (0x54C7D0..0x54C7F1); jmp5_len 6 but bytes 4..5 are "jne rel8":
 * replace the whole function, do not trampoline. */
#define DX_FN_REQUEST          0x0054C7D0u
#define DX_FN_REQUEST_ORIG     "80 79 09 00 75 19 b0 01 88 41 09 88 41 08 8b 44 24 04 50 51 8b 0d 40 b9 2a 01 e8 11 d8 ff ff c2 04 00"
#define DX_SITE_ENQUEUE_CALL   0x0054C7EAu /* "e8 11 d8 ff ff" call 0x54A000, the only enqueue call site */

/* PCDX11AsyncCreateResource::awaitResource(). thiscall ecx=item, no stack args, plain ret. [V]
 * Body 0x1F bytes. jmp5_len 6 (56 | 8b f1 | 8a 46 08), no relative operand in those 6. */
#define DX_FN_AWAIT            0x0054C800u
#define DX_FN_AWAIT_ORIG       "56 8b f1 8a 46 08 84 c0 74 13 57 8b 3d a0 12 a8 00 6a 01 ff d7 8a 4e 08 84 c9 75 f5 5f 5e c3"
#define DX_SITE_AWAIT_SLEEP    0x0054C811u /* "6a 01" push 1 (Sleep arg) */

/* cancelOrAwait() used by destructors. thiscall ecx=item, no stack args, plain ret. [V]
 * if (requested) { if (!deviceManager->dequeueCancel(item)) while (pending) Sleep(1); }
 * Body 0x35 bytes. jmp5_len 7 (56 | 8b f1 | 80 7e 09 00), next insn is "je rel8". */
#define DX_FN_CANCEL_OR_AWAIT  0x0054C850u
#define DX_FN_CANCEL_OR_AWAIT_ORIG "56 8b f1 80 7e 09 00 74 2a 8b 0d 40 b9 2a 01 56 e8 fb d7 ff ff 84 c0 75 1a 8a 46 08 84 c0 74 13 57 8b 3d a0 12 a8 00 6a 01 ff d7 8a 4e 08 84 c9 75 f5 5f 5e c3"
#define DX_SITE_CANCEL_SLEEP   0x0054C877u /* "6a 01" */

/* PCDX11DeviceManager::enqueueCreate(item, arg). thiscall ecx=deviceManager, 2 stack args, ret 8. [V]
 * jmp5_len 9 (56 | 8b f1 | 8b 86 a4 a1 00 00), no relative operand. */
#define DX_FN_ENQUEUE          0x0054A000u
#define DX_FN_ENQUEUE_ORIG16   "56 8b f1 8b 86 a4 a1 00 00 6a ff 50 ff 15 84 12"

/* PCDX11DeviceManager::dequeueCancel(item) -> bool removed. thiscall ecx=deviceManager, 1 arg, ret 4. [V]
 * jmp5_len 5 (53 56 57 8b f9). */
#define DX_FN_DEQUEUE_CANCEL   0x0054A060u
#define DX_FN_DEQUEUE_CANCEL_ORIG16 "53 56 57 8b f9 8d 9f 84 a1 00 00 53 ff 15 1c 12"

/* Worker thread proc (LPTHREAD_START_ROUTINE, stdcall, arg = deviceManager, ret 4). [V] */
#define DX_FN_WORKER           0x0054A100u
#define DX_FN_WORKER_ORIG16    "83 ec 08 53 55 56 8b 74 24 18 8b 86 a8 a1 00 00"
#define DX_SITE_WORKER_WAIT    0x0054A12Cu /* call [WaitForMultipleObjects] */
#define DX_SITE_WORKER_SLEEP   0x0054A1EDu /* "6a 01" Sleep(1) every 51 queue 2 releases */
#define DX_FN_WORKER_START     0x0054A3A0u /* fastcall ecx=deviceManager: creates sems, CS, thread */
#define DX_FN_WORKER_STOP      0x0054A440u

/* Create vfuncs: vtbl[0] of the AsyncCreateResource subobject. thiscall ecx=item, 1 arg, ret 4. [V] */
#define DX_FN_PS_ASYNC_CREATE  0x0054C8A0u /* ID3D11Device::CreatePixelShader (vtbl +0x3C) */
#define DX_FN_PS_ASYNC_CREATE_ORIG16 "a1 40 b9 2a 01 8b 40 24 8b 10 8b 52 3c 56 8d 71"
#define DX_FN_VS_ASYNC_CREATE  0x00553600u /* ID3D11Device::CreateVertexShader (vtbl +0x30) */
#define DX_FN_VS_ASYNC_CREATE_ORIG16 "a1 40 b9 2a 01 8b 40 24 8b 10 8b 52 30 56 8d 71"
#define DX_FN_BITMAPTEX_ASYNC_CREATE 0x0052FF00u /* CreateTexture2D/3D; ecx = item at tex+0x30 */
#define DX_FN_TEXTURE_ASYNC_CREATE   0x00547620u /* PCDX11Texture override: calls 0x52FF00 then frees staging */
#define DX_FN_STATICCB_ASYNC_CREATE  0x005307F0u /* CreateBuffer; item at cb+0x14 */

/* Shader request helpers [V] */
#define DX_FN_PS_REQUEST       0x005535F0u /* fastcall-like thiscall ecx=shader: d3d=0; request(0). ret (no args) */
#define DX_FN_PS_REQUEST_ORIG  "6a 00 c7 41 18 00 00 00 00 e8 d2 91 ff ff c3"
#define DX_FN_VS_INTERNAL_CREATE 0x00553690u /* InternalResource vtbl[1] of VS, ecx = shader+0xC */
#define DX_FN_PRECREATE_ONE    0x0054C8D0u /* thiscall ecx=PS: if (!d3d) request(0); plain ret */
#define DX_FN_PRECREATE_ONE_ORIG16 "83 79 18 00 c6 05 63 b9 2a 01 01 75 0e 6a 00 c7"

/* PCDX11StateManager bind wrappers. thiscall ecx=StateManager, 1 arg (shader*), ret 4. [V] */
#define DX_FN_SET_PS           0x0054F890u
#define DX_FN_SET_PS_ORIG16    "56 8b 74 24 08 57 8b f9 3b b7 98 01 00 00 74 54"
#define DX_FN_SET_VS           0x0054F9C0u
#define DX_FN_SET_VS_ORIG16    "56 8b 74 24 08 57 8b f9 3b b7 9c 01 00 00 74 54"

/* Texture await on use: BitmapTexture vtbl[0] (fastcall-like thiscall ecx=tex, plain ret,
 * returns tex+0x128). [V] */
#define DX_FN_BITMAPTEX_GET    0x00530400u
#define DX_FN_BITMAPTEX_REQUEST 0x00530290u /* thiscall, 8 stack args, enqueues via 0x54C7D0 at 0x53037C */

/* Shader object lifetime [V] */
#define DX_FN_PS_DTOR          0x0054C910u /* thiscall ecx=PS, plain ret (non deleting body) */
#define DX_FN_PS_DTOR_ORIG16   "56 8b f1 57 8d 7e 0c c7 06 6c 8b a9 00 c7 07 50"
#define DX_FN_PS_DELETING_DTOR_THUNK 0x0054C900u /* ecx = PS+0xC, arg flags, ret 4 */
#define DX_FN_SHADERMGR_RELEASE_PS 0x0054E5C0u /* thiscall ecx=ShaderManager, arg PS*, ret 4 */
#define DX_FN_SHADER_UNREF     0x0054E130u /* thiscall ecx=ShaderManager (unused), (shader, buckets), ret 8: refcount--, delete at 0, al=1 if deleted */
#define DX_FN_SHADER_UNREF_ORIG16 "83 ec 08 8b 54 24 0c 8b 42 1c 8b 48 08 8b 40 0c"
#define DX_FN_SHADERMGR_CREATE_PS 0x0054E570u /* thiscall ecx=ShaderManager, (blob, takeCopy, isWrapped), ret 0xC */
#define DX_FN_SHADERMGR_CREATE_VS 0x0054E5F0u

/* Shader tables and libraries [V] */
#define DX_FN_PSTABLE_CTOR     0x0054EE20u /* thiscall (blob, takeCopy, flag), ret 0xC */
#define DX_FN_PSTABLE_DTOR     0x0054EF40u /* scalar deleting dtor, thiscall ecx=table, arg flags, ret 4 */
#define DX_FN_PSTABLE_DTOR_ORIG16 "56 8b f1 c7 06 60 8c a9 00 a1 48 ab ea 00 85 c0"
#define DX_FN_PSTABLE_DESTROY_SHADERS 0x0054E870u /* fastcall-like thiscall ecx=table */
#define DX_FN_PSTABLE_QUEUE_ALL 0x0054EFA0u /* state=1, push every PS with d3d==0 to StateManager pending */
#define DX_FN_PSTABLE_QUEUE_USED 0x0054F000u /* state=2, gated by 0x54EA00 */
#define DX_FN_PSTABLE_QUEUE_USED_ORIG16 "51 a1 40 b9 2a 01 53 55 8b a8 50 01 00 00 57 8b"
#define DX_FN_PSTABLE_QUEUE_USED4 0x0054F080u /* state=3, gated, and only slots with (i&3) in 2 lighting variants */
#define DX_FN_PSTABLE_QUEUE_USED4_ORIG16 "83 ec 0c a1 40 b9 2a 01 53 8b d9 8b 88 50 01 00"
#define DX_FN_USAGE_LOOKUP     0x0054EA00u /* thiscall ecx=StateManager, (hashLo, hashHi) -> al, ret 8 */
#define DX_FN_USAGE_LOOKUP_ORIG16 "8b 81 d4 05 00 00 8b 91 cc 05 00 00 56 8b f0 8d"
#define DX_FN_USAGE_LOAD       0x00551530u /* thiscall ecx=StateManager, (uint64 list, 0 terminated), ret 4 */
#define DX_FN_VECTOR_PUSH_PTR  0x0054EC40u /* thiscall ecx=DxVector*, arg = &value (4 bytes), ret 4 */
#define DX_FN_SHADERLIB_CTOR   0x00546EA0u /* (blob, type, renderDevice): type 1 = PS (queues via 0x54F000) */
#define DX_FN_SHADERLIB_FILL   0x00547020u /* IShaderLib vtbl[1]: streamed lib, type 1 = PS (queues via 0x54F080) */
#define DX_FN_SHADERLIB_DTOR   0x00546E40u /* deletes table (vtbl[0](1)), frees buffer */
#define DX_FN_BUILTIN_PRECREATE 0x00539850u /* thiscall ecx=renderDevice, arg usage list (0 in shipped game) */
#define DX_SITE_LIBLOAD_QUEUE1 0x00546F0Eu /* call 0x54F000 in ShaderLib ctor */
#define DX_SITE_LIBLOAD_QUEUE2 0x005470B6u /* call 0x54F080 in ShaderLib::fill */
#define DX_SITE_USAGE_FILE     0x006185B6u /* ShaderUsageDX11.bin load decision area in FUN_006184F0 */

/* Render device frame and threads [V] */
#define DX_FN_FRAME            0x0053A170u /* thiscall ecx=renderDevice, arg char skipDraw, ret 4 */
#define DX_FN_FRAME_ORIG16     "55 8b ec 83 e4 f0 83 ec 64 a1 48 f1 e1 00 33 c4"
#define DX_SITE_DRAIN_BEGIN    0x0053A383u
#define DX_SITE_DRAIN_CALL     0x0053A467u /* call 0x54C8D0 */
#define DX_FN_RENDER_THREAD    0x0053BAD0u /* thread proc, arg renderDevice */
#define DX_FN_END_FRAME        0x0053C270u /* main thread: hands frame to render thread */
#define DX_FN_BEGIN_FRAME      0x00538D20u /* [?] main thread: processes deferred releases (0x538BA0) */
#define DX_FN_DEFERRED_RELEASE 0x00538BA0u /* ecx = renderDevice+0x10 */
#define DX_FN_RENDERDEVICE_CTOR 0x0053C370u

/* Frame pacing [V] */
#define DX_FN_CREATE_DEVICE    0x0054A930u
#define DX_SITE_MAX_LATENCY    0x0054AB83u /* "6a 01" push 1 -> IDXGIDevice1::SetMaximumFrameLatency (vtbl +0x30) */
#define DX_SITE_MAX_LATENCY_CTX "8b 51 30 6a 01 50 ff d2" /* at VA 0x54AB80, file 0x149F80 */
#define DX_FN_CREATE_SWAPCHAIN 0x0054D680u
#define DX_SITE_BUFFERCOUNT    0x0054D71Eu /* "38 43 19 b9 07 00 00 00 0f 95 c0 ... 83 c0 02" BufferCount = 2 + (tripleBuffer != 0) */
#define DX_FN_PRESENT          0x0054C9F0u /* thiscall ecx=RenderContext, 3 args, ret 0xC */
#define DX_SITE_PRESENT_CALL   0x0054CA2Cu /* "ff d2" IDXGISwapChain::Present(sync, 0) */

/* DFEngine usage [V] */
#define DX_FN_DF_INIT          0x00628210u /* thiscall ecx=DxDfManager, returns al */
#define DX_FN_DF_PER_FRAME     0x00628490u
#define DX_FN_DF_ZONE_CHANGE   0x00628130u /* thiscall (int zone), ret 4 */
#define DX_FN_DF_SHUTDOWN      0x00628420u
#define DX_FN_DF_CAMERA        0x00628060u
#define DX_FN_DF_GET_MANAGER   0x00628AA0u
#define DX_SITE_GETDFENGINE_CALL 0x00628292u /* "ff 15 40 10 a8 00" */

/* ------------------------------------------------------------------------ */
/* Generic containers                                                        */
/* ------------------------------------------------------------------------ */
/* cdc dtl vector [V]: when capacity == 0, data holds the dummy value 7. */
typedef struct DxVector {
    uint32_t size;      /* +0x0 */
    uint32_t capacity;  /* +0x4 */
    void    *data;      /* +0x8 */
} DxVector;
DX_CHECK_SIZE(DxVector, 0xC);

/* ------------------------------------------------------------------------ */
/* PCDX11AsyncCreateResource (vtable 0xA971B0 for the abstract base) [V]    */
/* ------------------------------------------------------------------------ */
#define DX_QUEUE_SLOTS        0x400u
#define DX_QUEUE_INDEX_TAKEN  0x400u /* worker sets item->queueIndex = 0x400 under the CS */

struct DxAsyncCreateResource;
typedef void (DX_THISCALL *DxAsyncCreateFn)(struct DxAsyncCreateResource *self, uint32_t arg);

typedef struct DxAsyncCreateVtbl {
    DxAsyncCreateFn asyncCreate; /* [0] the only slot. No destroy slot. */
} DxAsyncCreateVtbl;

typedef struct DxAsyncCreateResource {
    const DxAsyncCreateVtbl *vtbl; /* +0x0 */
    uint32_t queueIndex;           /* +0x4 ring slot, valid only if < 0x400 and ring[i].item == this.
                                      Uninitialized until first enqueue (constructors do not set it). */
    volatile uint8_t pending;      /* +0x8 "keepWaiting": set by request, cleared by worker (no lock) */
    volatile uint8_t requested;    /* +0x9 set once by request, never cleared */
    uint8_t  _padA[2];             /* +0xA */
} DxAsyncCreateResource;
DX_CHECK_SIZE(DxAsyncCreateResource, 0xC);
DX_CHECK_OFF(DxAsyncCreateResource, pending, 0x8);
DX_CHECK_OFF(DxAsyncCreateResource, requested, 0x9);

/* PCDX11InternalResource (device lost / reset list node) [V] */
typedef struct DxInternalResource {
    void   **vtbl;                         /* +0x0 [0] deleting dtor, [1] internalCreate, [2] internalRelease, [3] ?, [4],[5] nullsub */
    struct DxInternalResource *prev;       /* +0x4 (0xFFFFFFFF after unlink) */
    struct DxInternalResource *next;       /* +0x8 */
} DxInternalResource;
DX_CHECK_SIZE(DxInternalResource, 0xC);

/* ------------------------------------------------------------------------ */
/* Shaders                                                                   */
/* ------------------------------------------------------------------------ */
typedef struct DxDxbcWrapper {
    uint32_t magic;    /* +0x0 [R] */
    uint32_t size;     /* +0x4 total incl. this 16 byte header [R] */
    uint32_t hashLo;   /* +0x8 shader hash, key for dedup and ShaderUsage [V] */
    uint32_t hashHi;   /* +0xC [V] */
    /* DXBC follows */
} DxDxbcWrapper;

typedef struct DxShaderBinary {
    DxDxbcWrapper *wrapper;  /* +0x0 */
    const void    *bytecode; /* +0x4 = wrapper + 16 when wrapped */
    uint8_t        ownsData; /* +0x8 */
    uint8_t        flag9;    /* +0x9 [?] */
    uint16_t       size;     /* +0xA bytecode length (16 bit, passed as SIZE_T) [V] */
} DxShaderBinary;
DX_CHECK_SIZE(DxShaderBinary, 0xC);

/* cdc::PCDX11PixelShader, operator new size 0x3C [V]
 * vtables: async 0xA98B6C ([0]=0x54C8A0), internal 0xA98B50 ([0]=0x54C900 dtor thunk,
 * [1]=0x54C890 internalCreate returns 1 WITHOUT requesting (this is why PS are lazy),
 * [2]=0x549CD0 internalRelease, [3]=0x54C890). */
typedef struct DxPixelShader {
    DxAsyncCreateResource async;    /* +0x00 */
    DxInternalResource    internal; /* +0x0C */
    void                 *d3d;      /* +0x18 ID3D11PixelShader*, 0 until created */
    DxShaderBinary        bin;      /* +0x1C */
    uint32_t              refCount; /* +0x28 ShaderManager dedup refcount */
    struct DxPixelShader *hashNext; /* +0x2C ShaderManager bucket chain */
    DxVector              unk30;    /* +0x30 [?] freed in dtor; content unknown */
} DxPixelShader;
DX_CHECK_SIZE(DxPixelShader, 0x3C);
DX_CHECK_OFF(DxPixelShader, d3d, 0x18);
DX_CHECK_OFF(DxPixelShader, bin, 0x1C);
DX_CHECK_OFF(DxPixelShader, refCount, 0x28);

/* cdc::PCDX11VertexShader, size 0x30 [V]. vtables: async 0xA98EB4 ([0]=0x553600),
 * internal 0xA98E98 ([1]=0x553690 requests immediately at construction). */
typedef struct DxVertexShader {
    DxAsyncCreateResource  async;    /* +0x00 */
    DxInternalResource     internal; /* +0x0C */
    void                  *d3d;      /* +0x18 ID3D11VertexShader* */
    DxShaderBinary         bin;      /* +0x1C */
    uint32_t               refCount; /* +0x28 */
    struct DxVertexShader *hashNext; /* +0x2C */
} DxVertexShader;
DX_CHECK_SIZE(DxVertexShader, 0x30);

#define DX_VTBL_PS_ASYNC     0x00A98B6Cu
#define DX_VTBL_PS_INTERNAL  0x00A98B50u
#define DX_VTBL_VS_ASYNC     0x00A98EB4u
#define DX_VTBL_VS_INTERNAL  0x00A98E98u
#define DX_VTBL_ASYNC_BASE   0x00A971B0u
#define DX_VTBL_INTERNAL_BASE 0x00A98B18u

/* Embedded AsyncCreateResource offsets in texture / buffer classes [V] */
#define DX_BITMAPTEX_ASYNC_OFF   0x30u  /* cdc::PCDX11BitmapTexture: item at +0x30, pending at +0x38 */
#define DX_BITMAPTEX_RESOURCE    0x128u /* ID3D11 resource pointer returned by 0x530400 */
#define DX_TEXTURE_ASYNC_OFF     0x34u  /* cdc::PCDX11Texture (BitmapTexture at +4) */
#define DX_STATICCB_ASYNC_OFF    0x14u  /* cdc::PCDX11StaticConstantBuffer */
#define DX_VTBL_BITMAPTEX_ASYNC  0x00A9714Cu
#define DX_VTBL_TEXTURE_ASYNC    0x00A98238u
#define DX_VTBL_PROCTEX_ASYNC    0x00A982A0u
#define DX_VTBL_STATICCB_ASYNC   0x00A971D0u

/* PCDX11{Pixel,Vertex}ShaderTable [V]: PS table is 0x20 bytes, VS table 0x1C. */
typedef struct DxShaderTable {
    void     **vtbl;        /* +0x00 PS 0xA98C60 ([0]=0x54EF40), VS 0xA98C58 ([0]=0x54EDD0) */
    uint8_t   *blob;        /* +0x04 */
    uint32_t  *offsets;     /* +0x08 = blob + 8; 0xFFFFFFFF = empty slot */
    uint32_t   count;       /* +0x0C = blob[0] >> 2 */
    uint32_t   unk10;       /* +0x10 */
    void     **shaders;     /* +0x14 DxPixelShader* / DxVertexShader* per slot (may be 0) */
    uint8_t    ownsBlob;    /* +0x18 takeCopy */
    uint8_t    _pad19[3];
    uint32_t   precreateState; /* +0x1C PS table only: 0 none, 1 all queued, 2 gated, 3 gated+variant */
} DxShaderTable;
DX_CHECK_OFF(DxShaderTable, shaders, 0x14);
DX_CHECK_OFF(DxShaderTable, precreateState, 0x1C);

/* cdc::PCDX11ShaderLib, 0x18 bytes [V/R] */
typedef struct DxShaderLib {
    void         **vtblLib;     /* +0x00 IShaderLib 0xA980E4: [0]=0x546E40 dtor, [1]=0x547020 fill */
    void         **vtblExt;     /* +0x04 RenderExternalResource 0xA980DC: [0]=0x546DD0 dtor thunk */
    void          *extNext;     /* +0x08 [?] deferred release link */
    void          *renderDevice;/* +0x0C */
    DxShaderTable *table;       /* +0x10 */
    uint8_t       *buffer;      /* +0x14 streaming buffer, freed after fill completes */
} DxShaderLib;
DX_CHECK_SIZE(DxShaderLib, 0x18);

/* PCDX11ShaderManager (deviceManager+0x14C points to it) [V] */
#define DX_SHMGR_PS_LIVE      0x004u
#define DX_SHMGR_PS_UNIQUE    0x008u
#define DX_SHMGR_PS_PEAK      0x00Cu
#define DX_SHMGR_VS_LIVE      0x010u
#define DX_SHMGR_PS_BUCKETS   0x040u /* DxPixelShader*[257]: index hashLo & 0xFF, 256 for a special hash */
#define DX_SHMGR_VS_BUCKETS   0x444u
#define DX_SHMGR_HS_BUCKETS   0x848u

/* ------------------------------------------------------------------------ */
/* PCDX11StateManager (deviceManager+0x150), operator new size 0x600 [V]     */
/* ------------------------------------------------------------------------ */
typedef struct DxUsageHash { uint32_t lo, hi; } DxUsageHash;

typedef struct DxStateManager {
    DxInternalResource internal;     /* +0x000 vtbl 0xA98D34 */
    uint32_t  renderThreadId;        /* +0x00C written by FUN_0053A170 each frame */
    void     *d3dContext;            /* +0x010 ID3D11DeviceContext* (vtbl +0x24 PSSetShader, +0x2C VSSetShader) */
    uint8_t   _pad014[0x198 - 0x014];
    DxPixelShader  *currentPS;       /* +0x198 */
    DxVertexShader *currentVS;       /* +0x19C */
    uint8_t   _pad1A0[0x5CC - 0x1A0];
    DxVector  usage;                 /* +0x5CC DxUsageHash[] from ShaderUsageDX11.bin (empty in shipped game) */
    uint8_t   _pad5D8[0x5EC - 0x5D8];
    DxVector  pendingPS;             /* +0x5EC DxPixelShader*[] precreate list, popped LIFO by render thread */
    uint8_t   _pad5F8[0x600 - 0x5F8];
} DxStateManager;
DX_CHECK_SIZE(DxStateManager, 0x600);
DX_CHECK_OFF(DxStateManager, d3dContext, 0x10);
DX_CHECK_OFF(DxStateManager, currentPS, 0x198);
DX_CHECK_OFF(DxStateManager, usage, 0x5CC);
DX_CHECK_OFF(DxStateManager, pendingPS, 0x5EC);

/* ------------------------------------------------------------------------ */
/* PCDX11DeviceManager (global *0x12AB940) [V]                              */
/* ------------------------------------------------------------------------ */
typedef struct DxQueueSlot {
    DxAsyncCreateResource *item; /* +0 0 = cancelled/empty, worker skips */
    uint32_t               arg;  /* +4 passed to item->vtbl->asyncCreate */
} DxQueueSlot;
DX_CHECK_SIZE(DxQueueSlot, 8);

/* CRITICAL_SECTION is 24 bytes on Win32; kept opaque so this header needs no windows.h. */
typedef struct DxCritSec { uint8_t raw[24]; } DxCritSec;

typedef struct DxDeviceManager {
    void   **vtbl;                  /* +0x000 [1]=+0x04 getDisplayConfig() -> DxDisplayConfig*;
                                               [7]=+0x1C called before each pending push and in RenderDevice dtor [?] */
    DxInternalResource *irHead;     /* +0x004 */
    DxInternalResource *irTail;     /* +0x008 */
    uint8_t  _pad00C[0x01C - 0x00C];
    void    *hD3D11;                /* +0x01C HMODULE */
    void    *hDXGI;                 /* +0x020 HMODULE */
    void    *d3dDevice;             /* +0x024 ID3D11Device* */
    void    *dxgiFactory;           /* +0x028 IDXGIFactory* */
    void    *d3dContext;            /* +0x02C ID3D11DeviceContext* (immediate) */
    uint8_t  inResourceWalk;        /* +0x030 set while 0x549F10 walks the IR list; 0x54C7B0 skips internalCreate when set */
    uint8_t  _pad031[3];
    uint8_t  config1[0x88];         /* +0x034 DisplayConfig (see DX_CFG_*) */
    uint8_t  config2[0x88];         /* +0x0BC */
    uint8_t  _pad144[0x14C - 0x144];
    void    *shaderManager;         /* +0x14C */
    DxStateManager *stateManager;   /* +0x150 */
    DxVector adapters;              /* +0x154 stride 0x438 */
    uint8_t  _pad160[0x180 - 0x160];
    DxQueueSlot createRing[0x400];  /* +0x180 */
    void    *releaseRing[0x2000];   /* +0x2180 IUnknown* (worker calls vtbl[2] = Release); 0 = thread exit */
    void    *hWorkerThread;         /* +0xA180 */
    DxCritSec cs;                   /* +0xA184 guards ring[].item and item->queueIndex (dequeue/cancel only) */
    volatile int32_t enqueueCount;  /* +0xA19C InterlockedIncrement; slot = (count-1) & 0x3FF */
    uint32_t dequeueIndex;          /* +0xA1A0 worker only */
    void    *hFreeSem;              /* +0xA1A4 semaphore(0x400,0x400), WaitForSingleObject INFINITE in enqueue */
    void    *hFilledSem;            /* +0xA1A8 semaphore(0,0x400), WFMO index 0 */
    volatile int32_t relEnqueueCount; /* +0xA1AC */
    uint32_t relDequeueIndex;       /* +0xA1B0 */
    void    *hRelFreeSem;           /* +0xA1B4 semaphore(0x2000,0x2000) */
    void    *hRelFilledSem;         /* +0xA1B8 WFMO index 1 */
} DxDeviceManager;
DX_CHECK_OFF(DxDeviceManager, d3dDevice, 0x024);
DX_CHECK_OFF(DxDeviceManager, inResourceWalk, 0x030);
DX_CHECK_OFF(DxDeviceManager, config1, 0x034);
DX_CHECK_OFF(DxDeviceManager, config2, 0x0BC);
DX_CHECK_OFF(DxDeviceManager, shaderManager, 0x14C);
DX_CHECK_OFF(DxDeviceManager, stateManager, 0x150);
DX_CHECK_OFF(DxDeviceManager, createRing, 0x180);
DX_CHECK_OFF(DxDeviceManager, releaseRing, 0x2180);
DX_CHECK_OFF(DxDeviceManager, hWorkerThread, 0xA180);
DX_CHECK_OFF(DxDeviceManager, cs, 0xA184);
DX_CHECK_OFF(DxDeviceManager, enqueueCount, 0xA19C);
DX_CHECK_OFF(DxDeviceManager, dequeueIndex, 0xA1A0);
DX_CHECK_OFF(DxDeviceManager, hFreeSem, 0xA1A4);
DX_CHECK_OFF(DxDeviceManager, hFilledSem, 0xA1A8);
DX_CHECK_OFF(DxDeviceManager, hRelFilledSem, 0xA1B8);

/* DisplayConfig byte offsets (relative to the struct returned by vtbl[1]) [R, V for 0x18/0x19/0x24] */
#define DX_CFG_ENABLE_VSYNC          0x18
#define DX_CFG_ENABLE_TRIPLE_BUFFER  0x19
#define DX_CFG_DONT_DEFER_SHADERS    0x24
#define DX_CFG_USE_SHADER_USAGE      0x25
#define DX_DM_PRESENT_FORCE_SYNC     0x0B4 /* [?] deviceManager int; nonzero forces Present SyncInterval 1 */

/* ------------------------------------------------------------------------ */
/* PCDX11RenderDevice byte offsets (renderDevice = *(0xEAAB48) - 0x10) [V]  */
/* ------------------------------------------------------------------------ */
#define DX_RD_DEFERRED_HEAD_OLD    0x1E8    /* release list, freed by 0x538BA0 (two generations) */
#define DX_RD_DEFERRED_HEAD_NEW    0x1EC
#define DX_RD_DEFERRED_TAIL        0x1F0
#define DX_RD_FRAME_THREAD_ID      0x11A8   /* GetCurrentThreadId() of last FUN_0053A170 caller */
#define DX_RD_CREATOR_THREAD_ID    0x11AC   /* main thread (set in ctor) */
#define DX_RD_BUILTIN_PS_TABLES    0x10D70  /* 5 embedded DxShaderTable (0x20 each): 0x10D70,0x10D90,0x10DB0,0x10DD0,0x10DF0 */
#define DX_RD_PSTABLE_REGISTRY     0x111310 /* DxVector of PS tables (only if DontDeferShaderCreation); never consumed */
#define DX_RD_VSTABLE_REGISTRY     0x11131C
#define DX_RD_DRAIN_ENABLE         0x111358 /* uint8, set 1 in ctor, never cleared */
#define DX_RD_RENDER_RUN_FLAG      0x10D00
#define DX_RD_EVT_RENDER_GO        0x111598 /* render thread waits on this */

/* Precreate drain limits in FUN_0053A170 [V] */
#define DX_DRAIN_MAX_ITEMS     50
#define DX_DRAIN_MAX_SECONDS   0.1     /* double at VA 0xA89800 */

/* ------------------------------------------------------------------------ */
/* DFEngine interface (DoubleFusion). All methods __stdcall with explicit   */
/* this, callee cleans. Verified against DFEngine_orig.dll ret sizes. [V]   */
/* ------------------------------------------------------------------------ */
struct IDFEngine;
typedef struct IDFEngineVtbl {
    void *slot00;                                                             /* +0x00 ret 4 [?] dtor */
    int  (DX_STDCALL *Start)(struct IDFEngine *, uint32_t version, void *params); /* +0x04 ret 0xC; 0 = ok; exe passes 0x04050000 */
    void (DX_STDCALL *Release)(struct IDFEngine *);                           /* +0x08 ret 4 */
    void *slot0C;                                                             /* +0x0C ret 0xC, unused by exe */
    int  (DX_STDCALL *CreateObjectInstance)(struct IDFEngine *, uint32_t a, uint32_t b, void *desc, uint32_t *outHandle); /* +0x10 ret 0x14 */
    int  (DX_STDCALL *DestroyObjectInstance)(struct IDFEngine *, uint32_t handle); /* +0x14 ret 8 */
    int  (DX_STDCALL *DestroyAllObjects)(struct IDFEngine *, uint32_t zero);  /* +0x18 ret 8 */
    int  (DX_STDCALL *RefreshCurrentCreatives)(struct IDFEngine *, uint32_t zoneName); /* +0x1C ret 8 */
    int  (DX_STDCALL *Update)(struct IDFEngine *);                            /* +0x20 ret 4, per frame */
    int  (DX_STDCALL *StartZone)(struct IDFEngine *, uint32_t zoneName);      /* +0x24 ret 8 */
    int  (DX_STDCALL *EndZone)(struct IDFEngine *, uint32_t zoneName);        /* +0x28 ret 8 */
    void *slot2C, *slot30, *slot34;                                           /* ret 4, 0x10, 0xC; unused */
    int  (DX_STDCALL *SetView)(struct IDFEngine *, const void *m);            /* +0x38 ret 8 [?] name */
    int  (DX_STDCALL *SetProjection)(struct IDFEngine *, const void *m);      /* +0x3C ret 8 [?] name */
    void *slot40, *slot44;                                                    /* ret 8, 8; unused */
    int  (DX_STDCALL *SetEnabled)(struct IDFEngine *, int flag);              /* +0x48 ret 8 [?] name; called with 1 and 0 */
    void *slot4C, *slot50, *slot54, *slot58, *slot5C;                         /* unused by exe; real vtable has 24 slots */
} IDFEngineVtbl;
DX_STATIC_ASSERT(offsetof(IDFEngineVtbl, SetEnabled) == 0x48, "DF slot 18");
DX_STATIC_ASSERT(sizeof(IDFEngineVtbl) == 0x60, "DF vtbl 24 slots");

typedef struct IDFEngine { const IDFEngineVtbl *vtbl; } IDFEngine;

/* GetDFEngine: no args, plain ret (cdecl and stdcall identical), returns IDFEngine* or NULL. [V]
 * NULL is handled by the exe: FUN_00628210 stores 0, never retries, all users null check. */
typedef IDFEngine *(*DxGetDFEngineFn)(void);

/* DF manager singleton at 0x1C78C84 [V] */
typedef struct DxDfManager {
    uint32_t   unk00;     /* +0x00 */
    IDFEngine *engine;    /* +0x04 */
    int32_t    zone;      /* +0x08 init -1 */
    uint32_t   unk0C;     /* +0x0C */
    void      *helper;    /* +0x10 0x70 byte exe object (FUN_006EFEF0), only created if Start succeeded */
    uint32_t   unk14;     /* +0x14 float */
    int32_t    state;     /* +0x18 init 2 */
    uint8_t    initDone;  /* +0x1C */
} DxDfManager;
DX_CHECK_OFF(DxDfManager, engine, 0x04);
DX_CHECK_OFF(DxDfManager, initDone, 0x1C);

#endif /* DXHR_ENGINE_H */
