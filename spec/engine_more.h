/*
 * engine_more.h: additions to engine.h for the VS precreate (vs_precreate.md)
 * and the streaming / frame stall analysis (streaming.md).
 * DXHRDC.exe stock sha256 8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1.
 * VAs (image base 0x400000); runtime = module base + DX_RVA(va).
 * Include after engine.h. Same confidence tags: [V] disassembly, [R] RTTI, [?] inferred.
 */
#ifndef DXHR_ENGINE_MORE_H
#define DXHR_ENGINE_MORE_H

#include "engine.h"

/* ------------------------------------------------------------------------ */
/* Vertex shader table (cdc::PCDX11VertexShaderTable), 0x1C bytes [V]       */
/* ------------------------------------------------------------------------ */
typedef struct DxVertexShaderTable {
    void           **vtbl;     /* +0x00 0xA98C58 ([0] = 0x54EDD0 scalar deleting dtor) */
    uint8_t         *blob;     /* +0x04 */
    uint32_t        *offsets;  /* +0x08 = blob + 8; 0xFFFFFFFF = empty slot */
    uint32_t         count;    /* +0x0C = blob[0] >> 2 */
    uint32_t         unk10;    /* +0x10 not written by the ctor [?] */
    DxVertexShader **shaders;  /* +0x14 slot array, 0 for empty slots */
    uint8_t          takeCopy; /* +0x18 forwarded to ShaderManager::createVS; no blob XOR */
    uint8_t          _pad19[3];
} DxVertexShaderTable;          /* no precreate state field (PS table has +0x1C) */
DX_CHECK_SIZE(DxVertexShaderTable, 0x1C);
DX_CHECK_OFF(DxVertexShaderTable, shaders, 0x14);
DX_CHECK_OFF(DxVertexShaderTable, takeCopy, 0x18);

#define DX_VTBL_VSTABLE          0x00A98C58u
#define DX_VTBL_SHADERTABLE_BASE 0x00A98C50u
#define DX_VTBL_HSTABLE          0x00A98C68u /* ctor 0x54F160 */
#define DX_VTBL_DSTABLE          0x00A98C70u /* ctor 0x54F2A0 */
#define DX_VTBL_CSTABLE          0x00A98C78u /* ctor 0x54F3E0 */

/* VS table and VS lifetime functions [V] */
#define DX_FN_VSTABLE_CTOR       0x0054ECE0u /* thiscall (blob, takeCopy, flag), ret 0xC */
#define DX_FN_VSTABLE_CTOR_ORIG8 "8b 44 24 04 8a 54 24 08"  /* jmp5_len 8, no relative operand */
#define DX_FN_VSTABLE_DTOR       0x0054EDD0u /* thiscall (flags), ret 4 */
#define DX_FN_VSTABLE_DTOR_ORIG8 "56 8b f1 c7 06 58 8c a9"
#define DX_FN_VSTABLE_DESTROY_SHADERS 0x0054E810u /* thiscall ecx=table, plain ret */
#define DX_FN_VSTABLE_REGISTRY_REMOVE 0x0054EA60u /* thiscall ecx=renderDevice, (table), ret 4 */
#define DX_FN_SHADERMGR_RELEASE_VS 0x0054E640u   /* thiscall ecx=ShaderManager, (vs), ret 4 */
#define DX_FN_SHADERMGR_FIND_OR_CREATE_VS 0x0054E040u /* thiscall, 5 args, ret 0x14 */
#define DX_FN_VS_CTOR            0x00553590u /* thiscall (blob, takeCopy, isWrapped), ret 0xC */
#define DX_FN_VS_CTOR_ORIG8      "56 8b f1 57 8d 7e 0c c7"
#define DX_FN_VS_DTOR            0x00553640u /* scalar deleting dtor, thiscall (flags), ret 4, returns this */
#define DX_FN_VS_DTOR_ORIG8      "56 8b f1 57 8d 7e 0c c7"  /* jmp5_len 7 (56 | 8b f1 | 57 | 8d 7e 0c) */
#define DX_FN_VS_DELETING_DTOR_THUNK 0x00553630u /* IR vtbl[0]: sub ecx,0xC; jmp 0x553640 */
#define DX_FN_VS_INTERNAL_CREATE_ORIG8 "83 c1 f4 6a 00 c7 41 18" /* 0x553690; call request at 0x55369C */
#define DX_FN_INTERNAL_RELEASE   0x00549CD0u /* IR vtbl[2]: Release COM at IR+0xC (shared PS/VS) */
#define DX_FN_INTERNAL_CREATE_IF_ALLOWED 0x0054C7B0u
#define DX_FN_SHADERBINARY_CTOR  0x0054DD90u
#define DX_FN_SHADERBINARY_DTOR  0x0054DCC0u

/* Call sites of the VS table ctor [V] */
#define DX_SITE_VSTABLE_CTOR_LIB   0x00546F54u /* ShaderLib ctor type 2, "e8 87 7d 00 00" */
#define DX_SITE_VSTABLE_CTOR_FILL  0x00547083u /* ShaderLib::fill buffer[0] == 0, "e8 58 7c 00 00" */
#define DX_SITE_VS_REQUEST_CALL    0x0055369Cu /* "e8 2f 91 ff ff" request(0) in 0x553690 */
#define DX_SITE_VS_CTOR_INTERNAL   0x005535DFu /* "e8 cc 91 ff ff" call 0x54C7B0 at end of VS ctor */
#define DX_SITE_SETVS_AWAIT        0x0054F9EAu /* call 0x54C800 in setVertexShader */
#define DX_SITE_VS_CREATE_CALL     0x0055361Eu /* "ff d2" ID3D11Device::CreateVertexShader */

/* ShaderManager VS fields (deviceManager+0x14C) [V]. DX_SHMGR_VS_LIVE (0x10) and
 * DX_SHMGR_VS_BUCKETS (0x444) are in engine.h. */
#define DX_SHMGR_VS_UNIQUE       0x014u
#define DX_SHMGR_VS_PEAK         0x018u
#define DX_SHMGR_SPECIAL_HASH_LO 0x012AB978u /* hash equal to this 64 bit value uses bucket 256 */
#define DX_SHMGR_SPECIAL_HASH_HI 0x012AB97Cu

/* Builtin ShaderLib slots in the render device (created by 0x531720) [V] */
#define DX_RD_BUILTIN_LIB_PS0    0x10BE4u
#define DX_RD_BUILTIN_LIB_VS0    0x10BE8u
#define DX_RD_BUILTIN_LIB_VS1    0x10BECu
#define DX_RD_BUILTIN_LIB_VS2    0x10BF0u
#define DX_RD_BUILTIN_LIB_VS3    0x10BF4u
#define DX_RD_BUILTIN_LIB_PS1    0x10BF8u
#define DX_RD_BUILTIN_LIB_PS2    0x10BFCu
#define DX_RD_BUILTIN_LIB_VS4    0x10C00u
#define DX_RD_BUILTIN_LIB_VS5    0x10C04u

/* ------------------------------------------------------------------------ */
/* Input layout: cdc::PCDX11StreamDecl, 0x70 bytes (partial) [V]            */
/* ------------------------------------------------------------------------ */
typedef struct DxStreamDecl {
    DxInternalResource internal;   /* +0x00 vtbl 0xA98E5C: [0]=0x552ED0 dtor, [1]=0x552CA0 create, [2]=0x552F00 release */
    void     **vtblCommon;         /* +0x0C 0xA98E54 (CommonStreamDecl subobject) */
    void      *a1;                 /* +0x10 ctor arg 1 */
    uint32_t   unk14;              /* +0x14 key low (set by the cache) */
    uint32_t   unk18;              /* +0x18 key high */
    void      *elements;           /* +0x1C D3D11_INPUT_ELEMENT_DESC* */
    uint32_t   numElements;        /* +0x20 */
    void      *inputLayout;        /* +0x24 ID3D11InputLayout*, 0 until created */
    void      *cache;              /* +0x28 owning StreamDeclCache */
    DxShaderBinary *vsBinary;      /* +0x2C VS bytecode used as the input signature */
    uint8_t    _pad30[0x60 - 0x30];
    uint32_t   a6;                 /* +0x60 ctor arg 6 */
    uint8_t    _pad64[0x70 - 0x64];
} DxStreamDecl;
DX_CHECK_SIZE(DxStreamDecl, 0x70);
DX_CHECK_OFF(DxStreamDecl, elements, 0x1C);
DX_CHECK_OFF(DxStreamDecl, inputLayout, 0x24);
DX_CHECK_OFF(DxStreamDecl, vsBinary, 0x2C);

#define DX_VTBL_STREAMDECL           0x00A98E5Cu
#define DX_FN_STREAMDECL_CTOR        0x00552E60u /* thiscall, 6 args, ret 0x18; creates the layout immediately */
#define DX_FN_STREAMDECL_CREATE      0x00552CA0u /* CreateInputLayout (device vtbl +0x2C), call at 0x552CCD */
#define DX_FN_STREAMDECL_RELEASE     0x00552F00u
#define DX_FN_STREAMDECLCACHE_BUILD2 0x00553080u /* thiscall ecx=rd+0x1112D4, (layout, vsBinary), ret 8 */
#define DX_FN_STREAMDECLCACHE_BUILD4 0x00553190u /* thiscall, 4 args, ret 0x10 */
#define DX_RD_STREAMDECL_CACHE       0x1112D4u
#define DX_FN_MODELDRAWABLE_DRAW     0x00541090u /* PCDX11ModelDrawable vtbl 0xA97B8C [0], thiscall, 2 args, ret 8 */

/* ------------------------------------------------------------------------ */
/* File system [V]                                                            */
/* ------------------------------------------------------------------------ */
#define DX_G_FILESYSTEM          0x00E3FBC8u /* FileSystem* (MSThreadedFileSystem in play); getter 0x49EF10 */
#define DX_FN_GET_FILESYSTEM     0x0049EF10u
#define DX_VTBL_MSTHREADEDFS     0x00A900BCu
#define DX_VTBL_THREADEDFS       0x00A8FD44u
#define DX_VTBL_RESOLVEDECOMP    0x00A9C9BCu
#define DX_VTBL_MSFS             0x00A9004Cu

/* FileSystem vtable byte offsets used by the game */
#define DX_FSV_CREATE_REQUEST    0x00u
#define DX_FSV_STATUS            0x2Cu /* 1 = busy */
#define DX_FSV_PROCESS_ALL       0x30u /* TFS 0x4A0570: main thread delivery */
#define DX_FSV_SYNC_ALL          0x34u /* TFS 0x49FA90: processAll + Sleep(10) until idle */
#define DX_FSV_SYNC_REQUEST      0x38u /* TFS 0x49FAD0 */
#define DX_FSV_PUSH_MODE         0x54u /* mode 2 = deliver on the FS thread */
#define DX_FSV_POP_MODE          0x58u
#define DX_FSV_LOCK              0x6Cu /* EnterCriticalSection(this+0xD51C) */
#define DX_FSV_UNLOCK            0x70u
#define DX_FSV_SLEEP             0x74u /* 0x4A2D90 jmp [Sleep] */

#define DX_FN_TFS_PROCESS_ALL    0x004A0570u
#define DX_FN_TFS_PROCESS_ALL_ORIG8 "55 8b ec 83 e4 f8 83 ec"
#define DX_FN_TFS_SYNC_ALL       0x0049FA90u
#define DX_FN_TFS_SYNC_ALL_ORIG8 "56 8b f1 8b 06 8b 50 2c"
#define DX_FN_TFS_SYNC_REQUEST   0x0049FAD0u
#define DX_FN_TFS_STATUS         0x0049FA40u
#define DX_FN_TFS_RECEIVER_DATA  0x0049FEB0u /* FS thread, copies into the 2 MiB buffer */
#define DX_FN_TFS_THREAD_LOOP    0x0049FB40u
#define DX_FN_MSTFS_CTOR         0x004A2E40u
#define DX_FN_MSFS_PROCESS_ALL   0x004A1F90u
#define DX_FN_MSFS_READ_SUBMIT   0x004A1210u
#define DX_FN_MSFS_READER_THREAD 0x004A1190u /* no Ghidra function */
#define DX_FN_FS_PUMP_PER_FRAME  0x0061DBE0u /* status + processAll, called at 0x634104 */
#define DX_SITE_FS_PUMP_CALL     0x00634104u

/* MSThreadedFileSystem byte offsets */
#define DX_TFS_INNER             0x0004u
#define DX_TFS_STAGING           0x0008u /* 2 MiB */
#define DX_TFS_STAGING_USED      0x000Cu
#define DX_TFS_REQUESTS          0x0010u /* 900 x DxTfsRequest */
#define DX_TFS_PENDING           0xC4FCu
#define DX_TFS_PENDING_COUNT     0xD30Cu
#define DX_TFS_MODE2_COUNT       0xD310u
#define DX_TFS_MODE2_THREAD      0xD314u
#define DX_TFS_ACTIVE_LIST       0xD31Cu
#define DX_TFS_DT_HISTORY        0xD328u
#define DX_TFS_BYTES_HISTORY     0xD418u
#define DX_TFS_BYTES_HISTORY_IDX 0xD508u
#define DX_TFS_THREAD            0xD514u
#define DX_TFS_EVENT             0xD518u
#define DX_TFS_CS                0xD51Cu
#define DX_TFS_CREATOR_TID       0xD63Cu
#define DX_TFS_SIZE              0xD640u

typedef struct DxTfsRequest {
    void    **vtbl;       /* +0x00 0xA8FDDC */
    void     *fs;         /* +0x04 */
    void     *inner;      /* +0x08 inner request */
    void     *receiver;   /* +0x0C real receiver (e.g. ResolveReceiver 0xA9C918) */
    struct DxTfsRequest *next; /* +0x10 */
    int32_t   refCount;   /* +0x14 */
    int32_t   state;      /* +0x18 2 receiving, 3 done, 4 cancelled */
    int32_t   unk1C;      /* +0x1C init 3 */
    uint8_t   started;    /* +0x20 */
    uint8_t   complete;   /* +0x21 */
    uint8_t   direct;     /* +0x22 deliver on the FS thread (mode 2) */
    uint8_t   _pad23;
    uint8_t  *bufData;    /* +0x24 data in the staging buffer */
    uint32_t  bufSize;    /* +0x28 */
    uint32_t  totalSize;  /* +0x2C */
    uint32_t  offset;     /* +0x30 */
    uint32_t  skip;       /* +0x34 */
} DxTfsRequest;
DX_CHECK_SIZE(DxTfsRequest, 0x38);
DX_CHECK_OFF(DxTfsRequest, direct, 0x22);
DX_CHECK_OFF(DxTfsRequest, bufData, 0x24);

/* Throttle tunables [V] */
#define DX_G_THROTTLE_DATASTREAM     0x00E3FC1Cu /* uint8, default 1 */
#define DX_G_THROTTLE_MIN_MBPS       0x00E3FC18u /* int32, default 10 */
#define DX_G_PTR_THROTTLE_DATASTREAM 0x00E3FD2Cu /* GameOption<bool>+0x104 -> 0xE3FC1C */
#define DX_G_PTR_THROTTLE_MIN_MBPS   0x00E3FE44u /* GameOption<int>+0x104 -> 0xE3FC18 */
#define DX_SITE_THROTTLE_DEFAULT_IMM 0x00A5FD15u /* imm8 of "6a 01" at 0xA5FD14 */
#define DX_SITE_MIN_MBPS_DEFAULT_IMM 0x00A5FD53u /* imm8 of "6a 0a" at 0xA5FD52 */
#define DX_SITE_TFS_BUDGET_FMUL      0x004A072Cu /* "dc 0d 00 98 a8 00" fmul [0xA89800] = 0.1 (shared double) */
#define DX_SITE_TFS_BUDGET_FLOOR     0x004A0736u /* "d9 05 c8 a9 a8 00" fld [0xA8A9C8] = 0.001f (shared float) */
#define DX_G_LOADING_FLAG            0x01C79DA0u /* != 0 during level load: throttle off, 0x62FE90 sleeps 1 ms */

/* Buffer and chunk size sites [V] */
#define DX_SITE_TFS_STAGING_ALLOC    0x004A0503u /* "68 00 00 20 00" */
#define DX_SITE_TFS_STAGING_CAP      0x0049FF5Fu /* "b9 00 00 20 00" */
#define DX_SITE_TFS_REQUEST_SLOTS    0x004A047Fu /* "b9 83 03 00 00" (899 + 1) */
#define DX_SITE_MSFS_READ_MAX_CMP    0x004A218Fu /* "3d 00 00 08 00" */
#define DX_SITE_MSFS_READ_MAX_SET    0x004A2196u /* "b8 00 00 08 00" */
#define DX_SITE_SYNCALL_SLEEP_IMM    0x0049FAAEu /* "6a 0a" */
#define DX_SITE_SYNCREQ_SLEEP_IMM    0x0049FAFEu /* "6a 0a" */
#define DX_SITE_UBEROBJ_SYNCALL      0x004EFC04u /* "ff d0" fs->syncAll() in 0x4EFBC0 */
#define DX_FN_UBEROBJ_SPAWN_CHILD    0x004EFBC0u
#define DX_FN_UBEROBJ_SPAWN_CHILD_ORIG8 "55 8b ec 83 e4 f0 81 ec"

/* Resolve (DRM) receivers: main thread in play [V] */
#define DX_FN_RESOLVE_RECEIVE        0x005A75E0u
#define DX_FN_RESOLVE_COMPLETE       0x005A8320u
#define DX_FN_RESOLVE_COMPLETE_ORIG8 "81 ec 04 02 00 00 a1 48"
#define DX_G_SECTION_HANDLERS        0x019E07ECu /* handler* per section type */

/* ------------------------------------------------------------------------ */
/* Textures, static buffers, deferred updates [V]                             */
/* ------------------------------------------------------------------------ */
#define DX_FN_TEXTURE_FILL           0x00547C60u /* PCDX11Texture vtbl[1], main thread memcpy */
#define DX_FN_BITMAPTEX_SETUP_REQUEST 0x00530290u
#define DX_FN_BITMAPTEX_DYN_UPDATE   0x0052FDE0u /* DeferredUpdate: Map(WRITE_DISCARD)/Unmap */
#define DX_FN_RENDERMODEL_CONSTRUCT  0x00540BE0u
#define DX_FN_STATICPOOL_CTOR        0x00547560u /* (rd, bindFlags, pageSize, heap) */
#define DX_FN_STATICPOOL_ALLOC       0x00547420u /* CreateBuffer DEFAULT page, no init data, call at 0x5474D2 */
#define DX_FN_STATIC_VB_CTOR         0x00552750u
#define DX_FN_STATIC_IB_CTOR         0x00552620u
#define DX_FN_STATIC_VB_UPDATE       0x005526B0u /* UpdateSubresource, call at 0x552703 */
#define DX_FN_STATIC_IB_UPDATE       0x00552560u /* UpdateSubresource, call at 0x5525BD */
#define DX_FN_DEFERRED_UPDATE_PUSH   0x005398B0u /* thiscall ecx=rd, (node), ret 4 */
#define DX_SITE_POOL_VB_PAGE         0x0053C694u /* "68 00 00 a0 00" 10 MiB */
#define DX_SITE_POOL_IB_PAGE         0x0053C6CEu
#define DX_RD_POOL_VB                0x10D10u
#define DX_RD_POOL_IB                0x10D14u

typedef struct DxDeferredUpdate {
    void **vtbl;                     /* [0] run(rd) on the render thread */
    struct DxDeferredUpdate *next;   /* +0x4 */
} DxDeferredUpdate;
DX_CHECK_SIZE(DxDeferredUpdate, 8);

/* Render device frame handshake (offsets from rd = *(0xEAAB48) - 0x10) [V] */
#define DX_RD_IMM_CONTEXT            0x111580u /* ID3D11DeviceContext* used by deferred updates */
#define DX_RD_DEFERRED_PENDING       0x111584u /* filled by 0x5398B0 during the main frame */
#define DX_RD_DEFERRED_HANDOFF       0x111588u /* moved here by EndFrame, consumed by 0x53A170 */
#define DX_RD_EVT_PRESENT_DONE       0x11158Cu /* EndFrame waits 500 ms (0x53C2E5) */
#define DX_RD_EVT_UPDATES_TAKEN      0x111590u /* BeginFrame waits INFINITE (0x538D56) */
#define DX_RD_EVT_DRAW_DONE          0x111594u /* EndFrame waits INFINITE (0x53C2D7) */
#define DX_RD_PRESENT_TIMEOUT_FLAG   0x1115A0u /* set when the 500 ms wait timed out */
#define DX_FN_BEGIN_FRAME_CALLSITE   0x006179A0u /* rd vtbl +0xC */
#define DX_FN_END_FRAME_CALLSITE     0x00618C32u /* rd vtbl +0x10 */
#define DX_SITE_PRESENT_WAIT_IMM     0x0053C2DFu /* "68 f4 01 00 00" 500 ms */

/* DeviceManager vtbl[7] resolved [V]: 0x549F60 waits dm+0x17C (manual-reset event
 * created signaled at 0x54C332, never reset) when called on dm+0x178 (creator thread). */
#define DX_DM_CREATOR_TID            0x178u
#define DX_DM_EVT_GATE               0x17Cu

#endif /* DXHR_ENGINE_MORE_H */
