# Vertex shader precreate (Hook B for VS)

Target: DXHRDC.exe stock sha256 `8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1`. All addresses are VAs (image base 0x400000, runtime = module base + RVA). Prologue bytes below were read from the exe file and match `binary/out/DXHRDC.objdump.txt`. Decompiles: `binary/out/decomp_vs_tables.c`. Layout additions: `spec/engine_more.h`. Tags as in SPEC.md: **[V]** verified in disassembly, **[R]** RTTI or reconstruction consistent with disassembly, **[?]** inferred.

## 0. Answer in one paragraph

There is no VS analogue of 0x54F000 / 0x54F080 and no "precreate state" field, because the engine already requests every vertex shader at construction: VS table ctor 0x54ECE0 creates each VS through ShaderManager 0x54E5F0, the VS ctor 0x553590 ends with `call 0x54C7B0` (internalCreateIfAllowed), which tail-jumps to InternalResource vtbl[1] 0x553690, which does `d3d = 0; request(0)` (call at 0x55369C). So every VS of a freshly loaded library sits in the single async FIFO (deviceManager ring) the moment the table exists, behind whatever textures and constant buffers were queued before it. The render thread binds VS through setVertexShader 0x54F9C0, which awaits (0x54C800, Sleep(1) loop) if the VS is still pending. A VS "Hook B" therefore does not need to discover shaders; it needs to take VS creates **out of the shared FIFO** and run them on the DLL pool, so they do not wait behind texture uploads. CreateVertexShader needs only the bytecode. Input layouts are a separate object (PCDX11StreamDecl), created **lazily and synchronously on the render thread at first draw** of a (vertex layout, VS signature) pair via ID3D11Device::CreateInputLayout at 0x552CCD (StreamDecl internalCreate 0x552CA0); that is a first-draw cost but it cannot be precreated from the VS table alone.

## 1. Objects and layouts

### 1.1 PCDX11VertexShader (0x30 bytes) [V]

Already in `engine.h` (`DxVertexShader`). Confirmations:

- `operator new(0x30)` at 0x54E0C5 (`6a 30`), ctor 0x553590.
- Vtables: async 0xA98EB4 (`[0]=0x553600`), InternalResource 0xA98E98: `[0]=0x553630` (deleting thunk `sub ecx,0xc; jmp 0x553640`), `[1]=0x553690` internalCreate, `[2]=0x549CD0` internalRelease (Release of the COM pointer at IR+0xC = VS+0x18), `[3]=0x54C890` (`mov al,1; ret`), `[4]`,`[5]=0x9A2B10` nullsub.
- +0x18 `ID3D11VertexShader*`, +0x1C ShaderBinary {+0x1C wrapper*, +0x20 bytecode*, +0x24 ownsData u8, +0x26 uint16 size}, +0x28 refCount (ctor sets 1 at 0x5535D8), +0x2C hash chain.
- There is no +0x30 vector (that is PS only).

### 1.2 PCDX11VertexShaderTable (0x1C bytes) [V]

RTTI `.?AVPCDX11VertexShaderTable@cdc@@`, vtbl 0xA98C58 (`[0]=0x54EDD0` scalar deleting dtor); base PCDX11ShaderTable vtbl 0xA98C50 (`[0]=0x54EBA0`).

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | vtbl | 0xA98C58 | 0x54ED12 |
| +0x04 | u8* | blob | 0x54ECEC |
| +0x08 | u32* | offsets = blob + 8, 0xFFFFFFFF = empty slot | 0x54ED0C, 0x54ED55 |
| +0x0C | u32 | count = blob[0] >> 2 | 0x54ED06, 0x54ED0F |
| +0x10 | u32 | not written by the VS ctor [?] | |
| +0x14 | DxVertexShader** | slot array (0 for empty slots), allocated `count*4` | 0x54ED3E, 0x54ED83 |
| +0x18 | u8 | takeCopy (ctor arg 2), passed to createVS | 0x54ED1F, 0x54ED6A |

- **No +0x1C state field.** The PS table is 0x20 bytes because of its precreate state at +0x1C; the VS table ends at +0x1C (`push 0x1c` alloc at 0x546F3D and 0x547070).
- Unlike the PS ctor 0x54EE20, the VS ctor does **not** XOR the blob when takeCopy is set; takeCopy is only forwarded to ShaderManager.
- Registry: if ctor arg 3 (flag) is 0 and `DisplayConfig+0x24` (dontDeferShaderCreation) is set, the table is pushed to `renderDevice+0x11131C` (0x54EDC2). Removed in the dtor by 0x54EA60. Nothing iterates it (same as the PS registry at +0x111310).

### 1.3 ShaderManager VS fields (deviceManager+0x14C) [V]

| off | meaning | evidence |
|---|---|---|
| +0x10 | live VS references (createVS ++, releaseVS --) | 0x54E5F3, 0x54E640 |
| +0x14 | unique VS objects (++ only when a new VS was constructed, -- when 0x54E130 deleted one) | 0x54E62E, 0x54E640 |
| +0x18 | peak of +0x10 | 0x54E5FC..0x54E602 |
| +0x444 | `DxVertexShader*[257]` buckets, index `hashLo & 0xFF`, index 256 when the hash equals the 64 bit value at 0x12AB978/0x12AB97C | 0x54E076..0x54E084, 0x54E102 |

No lock protects the buckets or counters. All callers run on the thread that loads shader libraries (main thread in gameplay, see 2.3). The pool must never call ShaderManager functions.

## 2. Where VS tables are built (the analogue of 0x54F000 / 0x54F080)

### 2.1 ShaderLib ctor 0x546EA0 (thiscall, args blob, type, renderDevice, ret 0xC) [V]

Jump table 0x547008 indexed by `type - 1`: `[0]=0x546EE1` PS (alloc 0x20, 0x54EE20, then 0x54F000), **`[1]=0x546F2D` VS**: alloc 0x1C, `0x54ECE0(blob, 0, 0)`, call at **0x546F54** (`e8 87 7d 00 00`), store table at lib+0x10. `[2]=0x546F63` hull table 0x54F160 (vtbl 0xA98C68), `[3]=0x546F95` domain 0x54F2A0 (0xA98C70), `[4]=0x546FC7` compute 0x54F3E0 (0xA98C78).

Builtin libraries are created by 0x531720 (type pushed right before the blob pointer): VS (type 2) libs at renderDevice +0x10BE8, +0x10BEC, +0x10BF0, +0x10BF4, +0x10C04, +0x10C00 (blobs in `.shad`, for example 0x1F45D40); PS (type 1) at +0x10BE4, +0x10BF8, +0x10BFC.

### 2.2 ShaderLib::fill 0x547020 (IShaderLib vtbl[1], thiscall, 4 args, ret 0x10) [V]

Streamed library. Copies each chunk into lib+0x14; on the last chunk (arg 4 != 0) switches on `buffer[0]` via jump table 0x547134: **`[0]=0x547061` VS**: alloc 0x1C, `0x54ECE0(buffer+4, 1, 0)`, call at **0x547083** (`e8 58 7c 00 00`); `[1]=0x54708D` PS (then 0x54F080); `[2]` hull; `[3]` domain. Then frees the buffer (0x547120).

### 2.3 Which thread [V]

`fill` is called from ShaderLibSection::receiveData 0x5AA0A0 (vtbl 0xA9CBC4 slot 6) as `lib->vtbl[1](data, size, offset, offset+size == total)`, which is called by ResolveReceiver::receiveData 0x5A75E0 case 6 (`section->vtbl+0x18`), which is called by ThreadedFileSystem::processAll 0x4A0570, which runs on the **main thread** from 0x61DBE0 (call at 0x634104 in the main loop 0x633BB0) or from the loading loops in 0x62FF60. Exception: while a "mode 2" is pushed on the threaded FS (0x4A0210, used by 0x73AD40 / 0x73AFF0, save load), requests get the direct flag (+0x22, set at 0x49FD03) and receivers run on the FS thread. So the VS ctor, the dedup lookup and the enqueue normally run on the main thread, and the enqueue happens synchronously inside the table ctor.

## 3. VS create path [V]

```
0x54ECE0 VS table ctor (main thread)
  for i: if offsets[i] != -1: shaders[i] = ShaderManager::createVS(blob + offsets[i], takeCopy, 1)  // call 0x54ED77
0x54E5F0 createVS: live++, peak; s = 0x54E040(...); if new: unique++                               // ret 0xC
0x54E040 findOrCreate: hash lookup in buckets (+0x444); hit: refCount++ (0x54E0EA), return existing
                       miss: new(0x30), 0x553590(blob, takeCopy, isWrapped) at 0x54E0DA, link at bucket head
0x553590 VS ctor: async vtbl, pending=0, requested=0, IR ctor 0x54C760 (appends to dm IR list, no lock),
                  ShaderBinary ctor 0x54DD90, d3d=0, refCount=1, then call 0x54C7B0 at 0x5535DF
0x54C7B0: if (!dm->byte[0x30]) jmp IR.vtbl[1]          // skipped during the device reset walk
0x553690: ecx -= 0xC; push 0; d3d = 0; call request 0x54C7D0 at 0x55369C; al = 1
0x54C7D0 request: if (!requested) { requested = 1; pending = 1; dm->enqueue(item, 0) }   // 0x54A000
worker 0x54A100 -> item->vtbl[0] = 0x553600(arg 0):
   dev = dm->d3dDevice (+0x24); dev->CreateVertexShader(VS+0x20 bytecode, (uint16)VS+0x26, NULL, &VS+0x18)
   (ID3D11Device vtbl +0x30, call edx at 0x55361E); then worker sets pending = 0
```

- The create arg is 0 (`push 0` at 0x553693) and 0x553600 ignores it.
- CreateVertexShader gets no class linkage and no input layout. **Creating a VS does not need the input layout.**
- 0x5535F0 (shared PS/VS requestShader, `d3d = 0; request(0)`) is only reached for a VS from setVertexShader when `requested == 0`, i.e. a VS constructed while `dm->byte[0x30]` was set.

### 3.1 Bind and wait (render thread) [V]

setVertexShader 0x54F9C0 (thiscall ecx=StateManager, arg VS, ret 4):

```
if (vs != sm->currentVS /*+0x19C*/) {
  if (vs) { if (!vs->requested) 0x5535F0(vs); if (vs->pending) 0x54C800(vs);     // await at 0x54F9EA
            sm->ctx(+0x10)->VSSetShader(vs->d3d, NULL, 0) /* vtbl +0x2C */; current = vs; }
  else      ctx->VSSetShader(NULL, NULL, 0) }
```

Callers include ModelDrawable::draw 0x541090 (PCDX11ModelDrawable vtbl 0xA97B8C slot 0, call at 0x5410CF) and the 11 sites listed in SPEC.md 3.3. These run inside the render frame 0x53A170.

## 4. Input layouts (PCDX11StreamDecl) [V]

- Class: `.?AVPCDX11StreamDecl@cdc@@`, primary vtbl 0xA98E5C = InternalResource `[0]=0x552ED0` deleting dtor, `[1]=0x552CA0` internalCreate, `[2]=0x552F00` internalRelease, `[3]=0x4A52F0`, `[4]=0x579080`, `[5]=0x9A2B10`; CommonStreamDecl subobject at +0xC (vtbl 0xA98E54, `[0]=0x552E50` thunk to 0x552ED0). Size 0x70 (alloc at 0x5533C6 `6a 70`).
- Ctor 0x552E60 (thiscall, 6 args, ret 0x18): IR ctor 0x54C760, +0x10 a1, +0x1C a2 (D3D11_INPUT_ELEMENT_DESC*), +0x20 a3 (count), +0x24 = 0 (ID3D11InputLayout*), +0x2C a4 (**the VS ShaderBinary***), +0x28 a5 (cache), +0x60 a6, then `call 0x54C7B0` at 0x552EBC.
- internalCreate 0x552CA0: `dev->CreateInputLayout(+0x1C, +0x20, bin->bytecode (+0x4), (uint16)bin->size (+0xA), &this->+0x24)` through ID3D11Device vtbl +0x2C, `call edx` at 0x552CCD. Returns `hr == 0`.
- internalRelease 0x552F00: Release +0x24, zero it.
- Built by StreamDeclCache 0x553080 (thiscall ecx = cache at renderDevice+0x1112D4, args layout, VS ShaderBinary, ret 8; ModelDrawable::draw passes `vs + 0x1C`, call at 0x5411E3) and 0x553190 (layout, VS signature, flag, VS ShaderBinary). Both hash the pair, look it up (0x57D6E0), and on a miss build the element array, `new` the StreamDecl and call the ctor (0x553138 / 0x5533F0) on the calling thread. 0x553190 can also create a small SimpleStaticVertexBuffer for default stream values (0x547330 at 0x5534A9).
- Callers: draw functions 0x536F80, 0x537240, 0x5376F0, 0x5379E0, 0x537D30 (0x553190) and 0x541090, 0x534930 (0x553080; 0x534930 runs from the RenderDevice ctor at 0x53C7ED).

Conclusion: input layouts are **lazy, on the render thread, synchronous, at the first draw** of each (layout, VS) pair. Under DXVK, CreateInputLayout only validates and records attributes (no SPIR-V compile), so it is a small cost; the expensive work at that first draw is the pipeline compile. Precreating layouts would require the mesh vertex formats, which only exist when meshes are drawn; not covered here.

## 5. Destruction and unload [V]

```
ShaderLib release is deferred (renderDevice list, freed by 0x538BA0 from BeginFrame 0x538D20 on the main thread)
~ShaderLib 0x546E40: table->vtbl[0](1)
0x54EDD0 ~VertexShaderTable (thiscall this, flags, ret 4):
   0x54EA60(renderDevice, table)        // remove from +0x11131C registry
   0x54E810: for each non-null slot: ShaderManager::releaseVS 0x54E640(vs)   // call at 0x54E83C
             free slot array, +0x14 = 0
   if (flags & 1) free(table)
0x54E640 releaseVS: live--; if (0x54E130(vs, sm+0x444)) unique--
0x54E130 (shared): find vs in bucket; if (--vs->refCount == 0) { unlink; vs->IR.vtbl[0](1) }   // call eax at 0x54E19C
   IR.vtbl[0] = 0x553630: sub ecx,0xC; jmp 0x553640
0x553640 ~VertexShader (thiscall this, flags, ret 4, returns this):
   vtbls reset; cancelOrAwait 0x54C850 (call at 0x553653)
   0x579080(IR): jmp IR.vtbl[2] = 0x549CD0 -> Release ID3D11VertexShader (VS+0x18)
   0x54DCC0(bin): free bytecode copy if owned
   0x54C790(IR): unlink from the dm IR list (0x549E80)
   if (flags & 1) free(this)
```

cancelOrAwait (SPEC 3.4) for a VS: `requested` is always 1, so it calls `dm->cancel` 0x54A060, which nulls the ring slot and returns true for any `queueIndex != 0x400`; if `queueIndex == 0x400` it returns false and the dtor spins `while (pending) Sleep(1)`.

## 6. Implementing a VS precreate in the DLL

Goal: VS creates must not wait in the shared FIFO behind textures, and the render thread must never sleep on a VS. Two workable designs; A is preferred.

### 6.1 Design A: divert VS requests at `request` (Hook A already replaces 0x54C7D0)

In the DLL's `request_impl(item, arg)`, after winning `requested` (the existing byte cmpxchg 0 to 1 on +0x9):

```
if (item->vtbl == base + RVA(0xA98EB4)) {        // PCDX11VertexShader async vtbl
    item->queueIndex = 0x400;                     // never in the ring
    item->pending = 1;                            // before publishing to the pool
    EnterCriticalSection(&g_listCs); list_push(item, KIND_VS); LeaveCriticalSection(&g_listCs);
    signal pool; return;
}
stock path (enqueue 0x54A000)
```

- Pool thread: under g_listCs pop an entry whose tombstone is clear, mark it claimed (in-flight), leave the CS, call `0x553600(item, 0)` (thiscall ecx=item, 1 arg, ret 4) or call `ID3D11Device::CreateVertexShader` directly with the same arguments, then store-release `item->pending = 0`.
- **Await (0x54C800, Hook A)**: a VS with `pending == 1` and `queueIndex == 0x400` may be unclaimed in the DLL list. The await hook must try to claim it from the DLL list under g_listCs (same claim the pool uses) and create inline on the render thread; only if a pool thread already claimed it, wait for `pending == 0` (spin with a short Sleep(0)/yield, not Sleep(1)). The ring check alone is not enough for VS entries.
- Callers of request for VS: 0x55369C (ctor, main thread, device reset walk) and 0x5535F9 (setVS fallback, render thread). For the render thread case create inline instead of queueing (the render thread waits anyway).
- No dedup state is needed per table: request is idempotent through `requested` (+0x9) and ShaderManager dedup returns the existing object (already requested) for a repeated hash.

### 6.2 Design B: steal after the table ctor (if request cannot be diverted)

Detour 0x54ECE0 (thiscall, args blob, takeCopy, flag, ret 0xC; prologue `8b 44 24 04 | 8a 54 24 08`, 8 bytes, no relative operand). After the stock ctor returns, for each `s = table->shaders[i]` with `s && !s->d3d && s->pending`:

```
EnterCriticalSection(&dm->cs /* +0xA184 */)
  i = s->queueIndex
  if (i < 0x400 && dm->createRing[i].item == s) { dm->createRing[i].item = 0; s->queueIndex = 0x400; claimed = 1 }
LeaveCriticalSection(&dm->cs)
if (claimed) push to the DLL list (pending stays 1)
```

- Safe against the stale-index window in SPEC 2.4 because the enqueue happened synchronously on this same thread inside the ctor.
- The worker skips nulled slots (`item == 0`) and still frees the semaphore count.
- Also hook 0x547083 / 0x546F54 only if a per-call-site policy is needed; detouring the ctor covers both.
- Same await requirement as design A.

### 6.3 Lifetime (both designs, required)

- **Hook ~VertexShader 0x553640** (thiscall ecx=VS, arg flags, ret 4, returns this; prologue `56 8b f1 57 8d 7e 0c` 7 bytes, no relative operand, identical bytes to ~PixelShader 0x54C910). Under g_listCs tombstone every list entry for this VS. **If the entry was not yet claimed, also set `pending = 0` before calling the stock dtor.** Reason: stock cancelOrAwait sees `requested == 1`, calls `dm->cancel`, which returns false for `queueIndex == 0x400`, and then spins until `pending == 0`; an unclaimed DLL entry would never clear it (deadlock). This is the key difference from the PS Hook B, where unclaimed list entries still have `requested == 0` and the dtor skips the wait.
- If a pool thread already claimed the VS, leave pending alone; the stock dtor waits for the pool to finish (pool must not touch the object after `pending = 0`).
- Device manager shutdown 0x54A440: reuse the existing hook (stop pool, clear list, wait in-flight).
- Optional cross check: 0x54E130 (refcount drop to 0) is shared by PS and VS; hooking ~VertexShader is sufficient.

### 6.4 Locks and threads

| state | owner | lock |
|---|---|---|
| VS bucket chain, refCount (+0x28), ShaderManager counters | library loading thread (main) | none: never touch from the pool |
| `requested` (+0x9) | first requester | byte cmpxchg (existing Hook A) |
| `pending` (+0x8) | set by requester, cleared by creator | store-release on clear |
| ring slot / `queueIndex` | dm worker and cancel | `dm->cs` (+0xA184, spin count 0x200) |
| DLL list entries | DLL | g_listCs |

ID3D11Device is free threaded (SPEC 2.4), and the VS create touches only the device.

## 7. Differences from the PS path

| aspect | PS (Hook B today) | VS |
|---|---|---|
| request time | lazy at first bind (IR vtbl[1] 0x54C890 is `mov al,1; ret`) | at construction (IR vtbl[1] 0x553690 requests) |
| state when the table exists | requested 0, pending 0, not queued | requested 1, pending 1, already in the FIFO |
| table iteration hook | 0x54F000 / 0x54F080 (state +0x1C) | none exists; use request diversion (A) or the ctor 0x54ECE0 (B) |
| table size / state field | 0x20, state at +0x1C | 0x1C, no state field |
| takeCopy blob XOR | yes (0x54EE20) | no |
| claim protocol | cmpxchg {pending, requested} 0x0000 to 0x0101 | requested already 1: divert in request (A) or steal from ring under dm CS (B) |
| dtor wait on unclaimed DLL entry | none (requested 0) | deadlock unless the dtor hook clears pending |
| create vfunc | 0x54C8A0, CreatePixelShader vtbl +0x3C | 0x553600, CreateVertexShader vtbl +0x30 |
| dtor | 0x54C910 (thunk 0x54C900) | 0x553640 (thunk 0x553630) |
| table dtor | 0x54EF40 -> 0x54E870 -> 0x54E5C0 | 0x54EDD0 -> 0x54E810 -> 0x54E640 |
| ShaderManager buckets / counters | +0x40, +0x4 live, +0x8 unique, +0xC peak | +0x444, +0x10 live, +0x14 unique, +0x18 peak |
| bind wrapper | 0x54F890 (current +0x198) | 0x54F9C0 (current +0x19C) |
| extra first-draw object | none | StreamDecl (input layout) per (layout, VS), lazy on the render thread |
| count in data | 14,583 PS blobs | 3,379 VS blobs (REPORT.md) |

## 8. Hook sites (bytes from the exe)

| VA | role | conv | first 8 bytes | 5-byte span |
|---|---|---|---|---|
| 0x54ECE0 | VS table ctor | thiscall, 3 args, ret 0xC | 8b 44 24 04 8a 54 24 08 | 8, clean |
| 0x54EDD0 | ~VS table | thiscall, 1 arg, ret 4 | 56 8b f1 c7 06 58 8c a9 | 9, absolute operand |
| 0x54E810 | VS table destroyShaders | thiscall ecx=table, ret | 56 8b f1 83 7e 14 00 74 | 7, then jcc |
| 0x54E640 | ShaderManager::releaseVS | thiscall, 1 arg, ret 4 | 56 8b f1 8b 4c 24 08 ff | 7, clean |
| 0x54E5F0 | ShaderManager::createVS | thiscall, 3 args, ret 0xC | 56 8b f1 ff 46 10 8b 46 | 6, clean |
| 0x54E040 | findOrCreate VS | thiscall, 5 args, ret 0x14 | 83 ec 14 8b 44 24 18 53 | 7, clean |
| 0x553590 | VS ctor | thiscall, 3 args, ret 0xC | 56 8b f1 57 8d 7e 0c c7 | 7, clean |
| 0x553600 | VS asyncCreate | thiscall, 1 arg, ret 4 | a1 40 b9 2a 01 8b 40 24 | 5, absolute operand |
| 0x553630 | VS IR deleting thunk | thiscall ecx=IR, 1 arg | 83 e9 0c e9 08 00 00 00 | 8, contains jmp rel32 |
| 0x553640 | ~VertexShader | thiscall, 1 arg, ret 4 | 56 8b f1 57 8d 7e 0c c7 | 7, clean |
| 0x553690 | VS internalCreate | thiscall ecx=IR, ret | 83 c1 f4 6a 00 c7 41 18 | 5, clean (call rel32 at +0xC) |
| 0x5535F0 | requestShader (PS/VS) | thiscall, ret | 6a 00 c7 41 18 00 00 00 | 9, clean |
| 0x54C7B0 | internalCreateIfAllowed | thiscall ecx=IR | a1 40 b9 2a 01 80 78 30 | 5, absolute operand |
| 0x54F9C0 | setVertexShader | thiscall, 1 arg, ret 4 | 56 8b 74 24 08 57 8b f9 | 5, clean |
| 0x546F54 | call VS table ctor (lib ctor) | call site | e8 87 7d 00 00 89 46 10 | rel32 |
| 0x547083 | call VS table ctor (fill) | call site | e8 58 7c 00 00 e9 8c 00 | rel32 |
| 0x55369C | call request from VS internalCreate | call site | e8 2f 91 ff ff b0 01 c3 | rel32 |
| 0x5AA0A0 | ShaderLibSection receiveData | thiscall (this unused), 4 args (id, data, size, offset), ret 0x10 | 8b 4c 24 04 33 d2 8b c1 | 6, clean |
| 0x552E60 | StreamDecl ctor | thiscall, 6 args, ret 0x18 | 56 8b f1 e8 f8 98 ff ff | 8, contains call rel32 |
| 0x552CA0 | StreamDecl internalCreate (CreateInputLayout) | thiscall, ret | 8b 51 2c a1 40 b9 2a 01 | 8, absolute operand |
| 0x553080 | StreamDeclCache build (layout, bin) | thiscall, 2 args, ret 8 | 83 ec 14 53 56 8b 74 24 | 5, clean |
| 0x553190 | StreamDeclCache build (4 args) | thiscall, 4 args, ret 0x10 | 55 8b ec 83 e4 f0 81 ec | 6, clean |
| 0x541090 | ModelDrawable::draw | thiscall, 2 args, ret 8 | 51 a1 40 b9 2a 01 53 55 | 6, absolute operand |

Absolute operands (0x12AB940 etc.) are relocated in memory; copy live bytes into trampolines.

## 9. Open points

1. 0x553190 arg meaning beyond (layout, VS signature hash, invert flag, bin) not fully decoded [?].
2. VS table +0x10 never written by the VS ctor; content unknown [?].
3. Whether any VS is constructed while `dm->byte[0x30]` is set (device reset walk) in normal play was not observed.
