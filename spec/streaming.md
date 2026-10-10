# Streaming, loading and frame stalls

Target: DXHRDC.exe stock sha256 `8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1`. VAs, image base 0x400000. Byte strings were read from the exe file. Decompiles: `binary/out/decomp_streaming.c`, `binary/out/decomp_hitch_candidates.c`. Tags: **[V]** verified in disassembly, **[R]** RTTI or reconstruction consistent with disassembly, **[?]** inferred. `rd` = PCDX11RenderDevice (`*(0xEAAB48) - 0x10`), `dm` = deviceManager (`*0x12AB940`).

## 0. Short answers

- **Loader architecture.** One global file system object (`*(0xE3FBC8)`, getter 0x49EF10) built as MSThreadedFileSystem over ResolveDecompressor over the archive / MSFileSystem. A dedicated FS thread reads and decompresses (CDRM) into a 2 MiB staging buffer; a reader thread does blocking `SetFilePointer` + `ReadFile` of at most 512 KiB at a time; job threads help decompression. **All consumers (DRM resolve, section construction, texture and mesh object creation, shader library build) run on the main thread**, inside `ThreadedFileSystem::processAll` 0x4A0570, called once per main loop iteration from 0x61DBE0 (call at 0x634104).
- **Throttle.** 0x4A0570 limits its work per call to `max(0.1 * avgFrameTime, 1 ms)`, but only between requests or chunks, only while `g_throttle_datastream` (default 1) is set, only when the measured throughput is at least `g_throttle_minimalDataThroughput` (default 10 MB/s), and never while the loading flag `0x1C79DA0` is set. A single `requestComplete` (whole DRM construct) is never split.
- **Textures** are created on the async worker by `CreateTexture2D/3D` **with initial data** (`D3D11_USAGE_IMMUTABLE`, bind SRV) inside the create vfunc 0x52FF00; the main thread only memcpys the streamed bytes into per-mip staging blocks (0x547C60) and frees nothing until the worker is done (0x547620 frees staging). Only dynamic textures are updated later by `Map(WRITE_DISCARD)/Unmap` on the render thread (0x52FDE0).
- **Mesh vertex and index buffers** live in 10 MiB `D3D11_USAGE_DEFAULT` pool pages created by `CreateBuffer(desc, NULL)` **on the main thread** (0x547420), and every mesh range is uploaded by `UpdateSubresource` **on the render thread** at the start of the next render frame (deferred update list, 0x5526B0 / 0x552560 run from 0x53A230).
- **Main and render threads are coupled every frame**: EndFrame 0x53C270 waits INFINITE for the previous render frame (0x53C2D7) and up to 500 ms for the previous Present (0x53C2E5); BeginFrame 0x538D20 waits INFINITE until the render thread took the deferred-update batch (0x538D56). A stall on either thread shows up as one long frame.
- **Top 3 stall candidates for 100 to 700 ms non-shader frames** (section 6): (1) `syncAll` 0x49FA90 (Sleep(10) loop until the whole FS queue is idle) entered from gameplay object spawning 0x4EFBC0 at 0x4EFC04 when a child object is not resident; (2) unsplit main-thread `ResolveReceiver::requestComplete` 0x5A8320 constructing a large streamed DRM (meshes with synchronous 10 MiB `CreateBuffer`, objects, dependent loads), with the throttle disabled whenever throughput is below 10 MB/s; (3) render-thread `awaitResource` 0x54C800 on textures / VS stuck behind a streaming burst in the single async FIFO (with the deferred `UpdateSubresource` burst at frame start as an add-on), propagated to the main thread by EndFrame.

## 1. Threads

| thread | entry | created at | priority | role |
|---|---|---|---|---|
| main | WinMain 0x4A7D90 -> main loop 0x633BB0 | process | normal | game tick + render submit (0x62FE90 at 0x633F68 in state 8), FS delivery 0x61DBE0 (0x634104), all DRM construct work, BeginFrame / EndFrame |
| render | 0x53BAD0 (arg rd) | 0x53CC98 in RenderDevice ctor 0x53C370 | normal | waits rd+0x111598, runs 0x53A170: deferred updates, draws, precreate drain, Present |
| async create worker | 0x54A100 (arg dm) | 0x54A42D in 0x54A3A0 | normal | FIFO 0x400 creates (PS, VS, textures, static CBs) + 0x2000 deferred COM releases |
| FS thread | 0x4A2E10 -> 0x49FB40 (arg MSThreadedFileSystem) | `CreateThread` at 0x4A2ED4 in 0x4A2E40 | 2 (HIGHEST), `6a 02` at 0x4A2EDA | lock TFS CS, inner->processAll (ResolveDecompressor 0x5A91A0 -> MSFileSystem 0x4A1F90), unlock, wait events (0x4A0DA0, WFMO INFINITE) |
| file reader | 0x4A1190 (no Ghidra function; mislabeled under 0x4A1180) | `CreateThread` at 0x4A1290 in 0x4A1210, once | 2 | `WaitForSingleObject(ev, INFINITE)`, `SetFilePointer` (IAT 0xA81064), `ReadFile` (0xA81060), `SetEvent` (0xA81280) |
| job threads | 0x4D3690 | `_beginthreadex(stack 0x20000)` in 0x4D3A00 | normal | job ring of 400 entries; used by CDRM decompression (0x5A8630 / 0x4D2EE0) and other job chains |
| others | 0x4522A0, 0x5B1550, 0x6AE3B0, 0x6AE690, 0xA09D40 | | | debug remote, Steam save storage, network, HTTP; not on frame paths |

## 2. File system stack

### 2.1 Construction (main loop 0x633BB0) [V]

- `0x49EDE0` (called first): `new(0x17F10)` ResolveDecompressor 0x5A9350(inner = current global) at 0x49EE03, stored to 0xE3FBC8 and 0xE3FBD0.
- `0x49EE20`: `new(0xD640)` MSThreadedFileSystem 0x4A2E40(inner = ResolveDecompressor) at 0x49EE43, stored to 0xE3FBC8 and 0xE3FBE4.
- So `fs = *(0xE3FBC8)` is the MSThreadedFileSystem (vtbl 0xA900BC); its `+0x4` is the ResolveDecompressor (vtbl 0xA9C9BC), whose `+0x4` is the archive layer over MSFileSystem (vtbl 0xA9004C).

### 2.2 MSThreadedFileSystem / ThreadedFileSystem (0xD640 bytes) [V]

Vtable 0xA900BC (ThreadedFileSystem base 0xA8FD44 shares slots 0..23):

| slot (+off) | fn | meaning |
|---|---|---|
| 0 (+0x00) | 0x4A03A0 | createRequest |
| 11 (+0x2C) | 0x49FA40 | status: 1 busy if pending requests or inner busy (takes the TFS CS) |
| 12 (+0x30) | **0x4A0570** | **processAll: deliver buffered data to receivers (main thread)** |
| 13 (+0x34) | **0x49FA90** | **syncAll: `while (status()==1) { processAll(); Sleep(10); }`** (`push 0xa` at 0x49FAAE, Sleep via slot 29) |
| 14 (+0x38) | 0x49FAD0 | syncRequest(req): same loop until `req->state == 3` (`push 0xa` at 0x49FAFE) |
| 21 (+0x54) | 0x4A0210 | push mode; mode 2 makes new requests "direct" (receivers on the FS thread) |
| 22 (+0x58) | 0x4A0260 | pop mode |
| 27 (+0x6C) / 28 (+0x70) | 0x4A2DF0 / 0x4A2E00 | Enter / LeaveCriticalSection(this+0xD51C) |
| 29 (+0x74) | 0x4A2D90 | `jmp [Sleep]` |
| 30 (+0x78) | 0x4A2DA0 | `GetCurrentThreadId() == this+0xD63C` |
| 32 (+0x80) | 0x4A2DD0 | `GetCurrentThreadId() == arg` |

Fields (from 0x4A0470, 0x4A2E40, 0x4A0570, 0x49FEB0):

| off | meaning |
|---|---|
| +0x0004 | inner FS (ResolveDecompressor) |
| +0x0008 | staging buffer, `alloc(0x200000)` (`68 00 00 20 00` at 0x4A0503) |
| +0x000C | bytes used in the staging buffer (capacity check `b9 00 00 20 00` at 0x49FF5F) |
| +0x0010 | 900 Request objects of 0x38 bytes (`b9 83 03 00 00` at 0x4A047F: 899 + 1) |
| +0xC4F0 | MSFileEventManager* (embedded at +0xD534), +0xC4F4 / +0xC4F8 events from it |
| +0xC4FC | pending request array, +0xD30C pending count |
| +0xD310 / +0xD314 | mode 2 nesting count / owner thread |
| +0xD318 / +0xD31C / +0xD320 | free list / active list / free count |
| +0xD324 | ThreadedFileSystem::Receiver (forwarder, vtbl 0xA8FE08) |
| +0xD328..+0xD417 | 59-entry frame time history (floats) |
| +0xD418..+0xD507 | 60-entry delivered-bytes history, index at +0xD508 |
| +0xD514 / +0xD518 / +0xD51C | FS thread handle / wake event / CRITICAL_SECTION |
| +0xD63C | creator (main) thread id |

Request (0x38): +0x4 fs, +0x8 inner request, +0xC real receiver, +0x10 next, +0x14 refcount, +0x18 state (2 receiving, 3 done, 4 cancelled), +0x1C 3, +0x20 started, +0x21 complete, **+0x22 direct** (set at 0x49FD03 only while a mode 2 is pushed), +0x24 / +0x28 buffered data pointer / size, +0x2C total, +0x30 offset.

### 2.3 Data flow per chunk [V]

1. Reader thread: one blocking `ReadFile` per request at a time (the MSFileSystem state machine 0x4A1F90 keeps a single read in flight).
2. FS thread, under the TFS CS: MSFileSystem::processAll 0x4A1F90 -> ResolveDecompressor receiver 0x5A8EF0 (CDRM, decompression jobs waited with 0x4D2EC0 -> JobChain wait 0x4D3B30) -> `ThreadedFileSystem::Receiver::receiveData` 0x49FEB0, which memcpys into the 2 MiB staging buffer (or calls the real receiver directly if +0x22). If the staging buffer is full it accepts less and the read stalls (back pressure from the main thread).
3. Main thread, under the same TFS CS for the whole call: `processAll` 0x4A0570 walks the pending array; for each request: `receiver->vtbl[1](req, size)` once, `receiver->vtbl[0](req, data, size, offset, &abort)` for buffered data, then on completion `receiver->vtbl[3](req)` (requestComplete) or `vtbl[2]` (failed), then compacts the staging buffer (memmove).

The TFS CS is held by the main thread for the entire delivery (lock at 0x4A0584, unlock at the end) and by the FS thread for each inner processAll (0x49FB40). Each side blocks the other for the length of its critical section; the main thread also takes it in every `status()` call from 0x61DBE0.

### 2.4 Throttle in 0x4A0570 [V]

```
avgDt  = (dt_now (*(0x1C68DD0)+0x10) + 59 previous dt) / 60.0        // fdiv [0xA8FE20] at 0x4A06BA
mbps   = mean over the nonzero entries of the 60-frame bytes history, >> 20, / avgDt
budget = max(avgDt * 0.1 /*[0xA89800] at 0x4A072C*/, 0.001f /*[0xA8A9C8] at 0x4A0736*/)
throttle = *g_throttle_datastream (ptr 0xE3FD2C, at 0x4A07B5)
        && (float)*g_throttle_minimalDataThroughput (ptr 0xE3FE44, at 0x4A07C3) <= mbps
        && *(int*)0x1C79DA0 == 0                                       // not loading (0x4A07D9)
for each pending request:
   if (throttle && elapsed > budget) stop
   deliver one batch of buffered data; on completion call requestComplete    // never interrupted
```

Consequence: when the measured rate drops below 10 MB/s (slow disk, or the main thread itself starving the pipeline), throttling switches off and the next call processes every pending request and completion in one frame.

## 3. What runs on the main thread when a DRM arrives [V]

`ResolveReceiver` (vtbl 0xA9C918): `[0]=0x5A75E0` receiveData, `[2]=0x5A66B0` failed, `[3]=0x5A8320` requestComplete, `[4]=0x5A6DD0`. Section handlers are the singletons in the table at 0x19E07EC indexed by section type (RenderResourceSection vtbl 0xA9CD44, ShaderLibSection 0xA9CBC4, ObjectSection 0xA9CB64, GenericSection 0xA9CAA4, AnimationSection 0xA9CB04, MaterialSection 0xA9CC24, CollisionMeshSection 0xA9CC84, ScriptSection 0xA9CE1C, ...).

- **receiveData 0x5A75E0** (state machine at this+0x10): case 0 parses the DRM header (version 0x15), copies the section table, opens dependency DRMs (`%s%s.drm`), and for each section calls `handler->vtbl+0x8` (allocate; for render resources 0x5A64B0 -> 0x5A6010 creates the resource object), states 2..7 copy relocation data and stream each section payload through `handler->vtbl+0x18` (RenderResourceSection 0x5A61E0 -> `resource->vtbl[1]`, ShaderLibSection 0x5AA0A0 -> `ShaderLib::fill`), and when a section is complete call `handler->vtbl+0x1C` and apply relocations (0x5A67E0).
- **requestComplete 0x5A8320**: for each section `handler->vtbl+0x24` (construct; RenderResourceSection 0x5A6230 -> `resource->vtbl[3]`), per-type `vtbl+0x54`, queues the dependent DRM requests (0x5A7350), 0x5A6E80, then the owner callback (this+0x6C). This is one call, not time sliced.
- Per resource type:
  - **PCDX11Texture** (vtbl 0xA98264): `[1]=0x547C60` fill: validates the `PCD9` header (0x39444350), allocates per-mip staging blocks (`0x4D4430`), memcpy each chunk; on the last byte calls `0x530290(...)`, which records the format and mip pointers and calls `request(staging)` 0x54C7D0 at 0x53037C, i.e. enqueues the async create with the staging pointers as `arg`.
  - **PCDX11RenderModel** (vtbl 0xA97B44): `[3]=0x540BE0` construct mesh: validates mesh data, for each buffer `new` + `PCDX11StaticVertexBuffer` 0x552750 / `PCDX11StaticIndexBuffer` 0x552620, which allocate from the static pools (section 5) and push a DeferredUpdate.
  - **PCDX11ShaderLib** (vtbl 0xA980E4): `[1]=0x547020` fill: on the last chunk builds the PS table (then Hook B 0x54F080) or the VS table (each VS enqueued at construction, see `vs_precreate.md`).
- Objects: UberObjectSection (vtbl 0xA95730, slot 9 = 0x4EFBC0) and UberObjectSection_DX3 (0xAAECC4, slot 9 = 0x704CA0 -> 0x4EFBC0) spawn child object instances; **0x4EFBC0 calls `fs->syncAll()` (call at 0x4EFC04) when the child object's data entry `0x4ED680(id)->+8` is null**, then creates the instance (0x60AAF0).

## 4. Textures [V]

- Async create vfunc `PCDX11BitmapTexture` 0x52FF00 (worker thread, ecx = item at tex+0x30, arg = staging pointer array or 0):
  - builds a `D3D11_SUBRESOURCE_DATA` array from `arg` (pitch from 0x52FBF0 / 0x52FB50) when arg != 0,
  - 2D: `{W, H, Mips, ArraySize, Format, Sample 1/0, Usage = 1 IMMUTABLE (2 DYNAMIC if tex+0xF4+0x30), Bind = 8 SRV, CPU = 0 (0x10000 WRITE if dynamic), Misc = 4 TEXTURECUBE if ArraySize == 6}` -> `ID3D11Device::CreateTexture2D` (vtbl +0x14); 3D path uses +0x18.
  - So **the upload happens inside the create call on the worker thread**; nothing is deferred to the render thread for static textures.
- PCDX11Texture override 0x547620 calls 0x52FF00 then frees the staging blocks (+0x114, +0x120, +0x124).
- Use: `BitmapTexture::get` 0x530400 (vtbl[0]) awaits (0x54C800 at 0x53040D) if pending, then returns tex+0x128. Draw code reaches it through the vtable on the render thread.
- Synchronous variant `0x547B50` (BitmapTexture ctor used by 0x5481D0): request then await on the calling thread (0x547C1F).
- Dynamic textures: DeferredUpdate subobject at tex+0x28 (vtbl 0xA97154 `[0]=0x52FDE0`) runs on the render thread: `ctx->Map(res, mip, D3D11_MAP_WRITE_DISCARD (4), 0, &m)` (vtbl +0x38), memcpy rows, `Unmap` (+0x3C), with `ctx = rd+0x111580`. Queued by 0x52FD20 -> 0x5398B0.

## 5. Mesh buffers and the deferred-update list [V]

- Pools created in the RenderDevice ctor: `PCDX11StaticPool` ctor 0x547560(rd, bindFlags, pageSize, heap): VB pool `push 0xa00000; push 1` at **0x53C694** stored at rd+0x10D10, IB pool `push 0xa00000; push 2` at **0x53C6CE** stored at rd+0x10D14. Page size 10 MiB.
- `PCDX11StaticPool::alloc` 0x547420 (main thread, from the mesh construct): suballocates (0x51D380); if the page does not exist yet, `ID3D11Device::CreateBuffer({ByteWidth = page, Usage = 0 DEFAULT, Bind = pool+0x10, CPU 0, Misc 0, Stride 0}, NULL, &page)` (vtbl +0xC, `call edx` at 0x5474D2). No initial data.
- `PCDX11StaticVertexBuffer` ctor 0x552750: `alloc`, store source pointer (+0x28), then `0x5398B0(rd, &this->deferredUpdate)` pushes onto `rd+0x111584` (singly linked through +4).
- EndFrame 0x53C270 moves `rd+0x111584` to `rd+0x111588`; the render frame 0x53A170 takes `rd+0x111588`, runs every entry's `vtbl[0]` (loop 0x53A230..0x53A23E), then `SetEvent(rd+0x111590)` at 0x53A24D.
- `vtbl[0]` of a static VB is 0x5526B0: `ctx->UpdateSubresource(page, 0, &box{offset, 0, 0, offset + count*stride, 1, 1}, src, 0, 0)` (vtbl +0xC0, call at 0x552703); IB 0x552560 (call at 0x5525BD).
- Net effect: **DEFAULT usage + UpdateSubresource on the render thread**, all meshes constructed since the previous frame are uploaded in one batch before the first draw. Under DXVK this is a host copy into staging memory plus a GPU copy per range [?] (cost proportional to bytes; DXVK may also throttle or flush when much staging memory is outstanding [?]).
- CreateBuffer on the main thread happens only when a new 10 MiB page is needed.

## 6. Candidate stall sites, ranked

| # | site (VA, bytes) | thread | why it can cost 100 to 700 ms | evidence |
|---|---|---|---|---|
| 1 | `syncAll` 0x49FA90 (`56 8b f1 8b 06 8b 50 2c`), entered at **0x4EFC04** (`ff d0`) from UberObject child spawn 0x4EFBC0 (`55 8b ec 83 e4 f0 81 ec`) | main | waits until the threaded FS has **no** pending request at all, processing everything (throttled per call) and sleeping 10 ms per iteration; cost = whole streaming backlog. Triggered whenever a spawned child's object DRM is not resident. | [V] code; frequency in hubs [?] |
| 2 | `ResolveReceiver::requestComplete` 0x5A8320 (`81 ec 04 02 00 00 a1 48`) called from 0x4A0570 (`55 8b ec 83 e4 f8 83 ec`) | main | one unsplit call constructs every section of a DRM: meshes (0x540BE0) with synchronous 10 MiB `CreateBuffer` pages (0x547420, `8b 44 24 08 83 ec 18 53`), textures handed to the FIFO, shader tables, object init callbacks, dependent requests. The throttle cannot split it and is disabled below 10 MB/s or while 0x1C79DA0 != 0. Also holds the TFS CS, stalling the FS thread. | [V] |
| 3 | `awaitResource` 0x54C800 (`56 8b f1 8a 46 08 84 c0`), Sleep at 0x54C813, reached from `BitmapTexture::get` 0x530400 (`56 8b f1 8a 46 38 8d 4e`), setVS 0x54F9C0, setPS 0x54F890 | render | a streaming burst puts hundreds of texture creates (each a full IMMUTABLE upload inside DXVK) and VS creates into the single FIFO worker; the first draw of a newly streamed object waits for the worker to reach its items. The render stall is then seen by the main thread in EndFrame (0x53C2D7). Hook A inline create helps only for items still in the ring. | [V] |
| 4 | deferred updates in 0x53A170 (`55 8b ec 83 e4 f0 83 ec`): 0x5526B0 (`83 ec 18 8b 51 14 56 8b`), 0x552560 (`83 ec 18 56 8b f1 8b 46`) | render | all VB/IB bytes of the meshes constructed in the previous main frame are copied in one go before drawing; large cells can be tens of MB. | [V] code, size [?] |
| 5 | `enqueue` 0x54A000 free-slot wait at 0x54A00C (`ff 15 84 12 a8 00`) | main (texture fill 0x53037C, VS ctor 0x55369C), render (lazy PS) | if more than 0x400 creates are outstanding the producer blocks INFINITE until the worker frees slots. | [V] |
| 6 | BeginFrame 0x538D20 (`83 ec 0c 53 56 8b f1 ff`) -> deferred releases 0x538BA0 | main | unit unload: destructors run here (cancelOrAwait 0x54C850 can sleep), COM releases go to worker queue 2 (Sleep(1) per 51 releases at 0x54A1F1, which also delays creates). | [V] code, magnitude [?] |
| 7 | TFS CS (this+0xD51C) between `status()` 0x49FA40 / processAll 0x4A0570 (main) and 0x49FB40 (FS thread) | main / FS | the FS thread holds it during decompression waits (0x4D2EC0 -> 0x4D3B30 INFINITE); the main thread blocks in 0x61DBE0. Bounded by one chunk of decompression. | [V] |
| 8 | EndFrame Present wait 0x53C2E5 (500 ms cap, `68 f4 01 00 00` at 0x53C2DF) | main | if the previous Present takes long the main thread waits up to 500 ms, then skips signalling the render thread (recovered by 0x53C220). | [V] |

Shader waits (lazy PS at first bind, DXVK pipeline compiles at draw) are excluded here; see SPEC.md and REPORT.md.

## 7. Tunables

| name | where read | storage / site | current value | notes |
|---|---|---|---|---|
| g_throttle_datastream | 0x4A05A6, 0x4A07B5, 0x4A0A01 via ptr 0xE3FD2C | bool 0xE3FC1C (BSS), default from `6a 01` at **0xA5FD14** (registered by 0xA5FD10 as GameOption<bool> at 0xE3FC28) | 1 | 0 disables the time budget entirely |
| g_throttle_minimalDataThroughput | 0x4A07C3 via ptr 0xE3FE44 | int 0xE3FC18 (BSS), default `6a 0a` at **0xA5FD52**; option limits 1, 200 (`c7 44 24 24 c8 00 00 00` at 0xA5FD63), 1, read as min, max, step [?] (0xA5FD30, GameOption<int> at 0xE3FD40) | 10 MB/s | throttle applies only when measured MB/s >= this |
| time budget factor | 0x4A072C `dc 0d 00 98 a8 00` | double at 0xA89800 | 0.1 (of avg frame time) | constant shared with the precreate drain limit in 0x53A170; patch the operand, not the constant |
| time budget floor | 0x4A0736 `d9 05 c8 a9 a8 00` | float at 0xA8A9C8 | 0.001 s | float shared by many functions |
| frame history | 0x4A06BA `dc 35 20 fe a8 00`; `b9 3c 00 00 00` at 0x4A0771 / 0x4A0A25 | double 60.0 at 0xA8FE20 | 60 frames | |
| loading flag | 0x4A07D9 `cmp [0x1C79DA0], ebp` | int 0x1C79DA0 | 1 during level load (0x62FF60), 0 in play | also makes 0x62FE90 Sleep(1) (0x62FF04) |
| TFS staging buffer | alloc `68 00 00 20 00` at 0x4A0503; capacity `b9 00 00 20 00` at 0x49FF5F | | 2 MiB | patch both together |
| TFS request slots | `b9 83 03 00 00` at 0x4A047F | | 900 | |
| max bytes per ReadFile | `3d 00 00 08 00` / `b8 00 00 08 00` at 0x4A218F / 0x4A2196 | | 512 KiB | single read in flight |
| MSFS ring | 0x4A215C, 0x4A2164, 0x4A219D, 0x4A21A4, 0x4A21CA, 0x4A21D1 (0x100000); 0x4A1FDC, 0x4A21BD (0x100800) | | 1 MiB (+2 KiB) | |
| sync loop sleep | `6a 0a` at 0x49FAAE (syncAll), 0x49FAFE (syncRequest) | | 10 ms | |
| static pool page | `68 00 00 a0 00` at 0x53C694 (VB), 0x53C6CE (IB) | | 10 MiB | DEFAULT buffers, created on demand on the main thread |
| async FIFO depth | dm ring | SPEC.md 2.1 | 0x400 creates, 0x2000 releases | |
| Present wait | `68 f4 01 00 00` at 0x53C2DF | | 500 ms | |
| FS / reader thread priority | `6a 02` at 0x4A2EDA / 0x4A1296 | | THREAD_PRIORITY_HIGHEST | |
| job threads | 0x4D3D40 via 0x4D2DD0: `HKCU\SOFTWARE\Eidos\Deus Ex: HRDC\ForceJobThreadCount` (default -1 = auto from the CPU probe 0x4D2DB0, count minus 1), `AllowJobStealing` (default 1) | registry | auto | the only streaming-related registry values found |
| debug menu options | registered in 0xA62040: "Streaming/New Deferred Release Feature" (object 0x1C68C20, name "DeferredStreaming", at 0xA674F0), "StreamGroup Streaming" (0x1C68BD0), "Stream" (0x1C68BC0), "CellPrediction" (0x1A07B3C), "Streaming/Introduce Scenario Instances Upon StreamGroup Load" (0x1C5D284) | | not traced [?] | |

No memory budget constant for streaming was found in the FS or resolve code; residency is driven by unit / cell lists.

## 8. Main / render frame handshake [V]

```
main (per frame, 0x62FE90 from 0x633F68):
  0x618F30 -> 0x617970 -> rd->BeginFrame (vtbl +0xC = 0x538D20)
        WFSO(rd+0x111590, INFINITE) at 0x538D56   // render thread took the last deferred-update batch
        0x538BA0 deferred releases
  game tick, render submit 0x618920 ... rd->EndFrame (vtbl +0x10 = 0x53C270, `call edx` at 0x618C32)
        WFSO(rd+0x111594, INFINITE) at 0x53C2D7   // previous render frame finished drawing
        WFSO(rd+0x11158C, 500) at 0x53C2E5        // previous Present returned
        rd+0x111588 = rd+0x111584 (deferred updates), ResetEvent(rd+0x111594), SetEvent(rd+0x111598)
  0x61DBE0: fs->status, fs->processAll (0x4A0570, all DRM work)
render (0x53BAD0 loop):
  WFSO(rd+0x111598, INFINITE)
  0x53A170: run deferred updates, SetEvent(rd+0x111590) at 0x53A24D, scenes and draws (awaits),
            0x538BF0, SetEvent(rd+0x111594) at 0x53A570, Present 0x54C9F0 at 0x53A584, SetEvent(rd+0x11158C)
```

So any render frame longer than the main frame stretches the main frame in EndFrame, and a long main frame (DRM construct, syncAll) delays the render frame (render waits rd+0x111598).

## 9. Sleep and wait sites (Q3)

Full list with bytes, thread, timeout and reachability: `binary/out/decomp_hitch_candidates.c` header. Per-frame relevant ones:

| call site | bytes | function | API (timeout) | thread | blocks per frame? |
|---|---|---|---|---|---|
| 0x53C2D7 | ff d6 | EndFrame 0x53C270 | WFSO INFINITE | main | yes, waits for the render thread |
| 0x53C2E5 | ff d6 | EndFrame 0x53C270 | WFSO 500 ms | main | yes, waits for Present |
| 0x538D56 | ff 15 84 12 a8 00 | BeginFrame 0x538D20 | WFSO INFINITE | main | yes, short normally |
| 0x54C813 | ff d7 | awaitResource 0x54C800 | Sleep(1) loop | render (also main in sync paths) | at first use of any pending resource |
| 0x54A00C | ff 15 84 12 a8 00 | enqueue 0x54A000 | WFSO INFINITE | producer | only with > 0x400 queued creates |
| 0x49FAB2 | ff d2 (Sleep 10 via 0x4A2D90) | syncAll 0x49FA90 | Sleep(10) loop | main | when 0x4EFC04 fires in play; otherwise load/init only |
| 0x4D3BF0 | ff 15 84 12 a8 00 | JobChain wait 0x4D3B30 | WFSO INFINITE | job waiter (FS thread for CDRM) | indirect, via the TFS CS |
| 0x54C879 | ff d7 | cancelOrAwait 0x54C850 | Sleep(1) loop | destroying thread | rarely |
| 0x53C235 | ff 15 84 12 a8 00 | pump 0x53C220 | WFSO 1 ms | main | only after a Present timeout |
| 0x62FF04 | ff 15 a0 12 a8 00 | 0x62FE90 | Sleep(1) | main | only while loading |
| 0x4A5660 / 0x4A590F | ff 15 a0 12 a8 00 | pump 0x4A55F0 | Sleep(100) | main | only while inactive [?] |
| 0x549F7A | ff 15 84 12 a8 00 | dm vtbl[7] 0x549F60 | WFSO INFINITE on dm+0x17C | main | never blocks: manual-reset event created signaled (0x54C332), never reset |

Not per frame: level-load Sleep(5) loops in 0x62FF60 (0x6300AF ...), unit waits 0x61FC33 / 0x61C4DC, Sleep(param) 0x602825, worker / FS / reader / job idle waits, save storage, network, GFx video, CRI movie, startup CPU probe. No `pause` spin loops exist; the only pure spin is 0x4D30A0 (Sleep(0) every 1000 spins while the 400-entry job ring is full). SleepEx, WaitFor*Ex, SignalObjectAndWait, MsgWaitForMultipleObjects and timeBeginPeriod are not imported.

## 10. Resolved items from SPEC.md open issues

- deviceManager `vtbl[7]` (+0x1C) is 0x549F60: on the dm creator thread (dm+0x178, set at 0x54C309) it waits on dm+0x17C, a manual-reset event created signaled at 0x54C332; no ResetEvent call site targets it, so it does not block.
- ShaderLib::fill runs on the main thread (inside 0x4A0570) except while a mode 2 is pushed on the threaded FS (save load), when receivers run on the FS thread.
- 0x538D20 is BeginFrame on the main thread: 0x62FE90 -> 0x618F30 -> 0x617970 -> rd vtbl +0xC (call at 0x6179A0).

## 11. Gaps

1. How often 0x4EFBC0 reaches the syncAll in hubs is not measured; a counter hook on 0x49FA90 with a caller address would settle it.
2. Callers of UberObjectSection slot 9 and of JobChainImplWithThreads::wait other than CDRM were not traced (vtable dispatch).
3. Sizes of typical hub DRMs (bytes per requestComplete, meshes per frame) need runtime logging.
4. DXVK-side costs of UpdateSubresource bursts and IMMUTABLE texture creation are inferred, not measured.
5. The 0x5CDE93 mutex (DAT_019E17C4) and the 0x9F0000-range waits belong to unidentified subsystems; they are not on traced per-frame paths.
6. 0x632770 (called each loop iteration at 0x63413F) is an integrity check that hashes strings at `*(0x1C79DA4)+0x10C` and spins forever on mismatch; irrelevant for timing but do not modify that data.
