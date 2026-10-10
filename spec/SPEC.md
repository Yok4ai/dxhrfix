# DXHRDC engine spec for the DFEngine.dll proxy hooks

Target: DXHRDC.exe, stock sha256 `8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1`, 32-bit, image base 0x400000, DYNAMIC_BASE set.

- Address rules: runtime address = `GetModuleHandleA(NULL) + RVA`, where RVA = VA - 0x400000. The .text file offset is VA - 0x400C00.
- Verify the original bytes before every patch. The byte strings are in `engine.h` (`*_ORIG*`).
- Confidence tags: **[V]** verified in this exe's disassembly. **[R]** matches RTTI or the rrika/cdcEngineDXHR reconstruction and agrees with the disassembly. **[?]** inferred or uncertain.
- Sources: the full objdump in `binary/out/DXHRDC.objdump.txt`, existing decompiles in `binary/out/*.c`, and a fresh Ghidra 12.1.4 headless run on a copy of `binary/proj` (scratchpad). Class names come from MSVC RTTI (cdcEngineDXHR objtree.txt).
- Globals:
  - `deviceManager` = `*(PCDX11DeviceManager**)0x12AB940` [V]. It is written once at 0x54C415.
  - `renderDevice` = `*(void**)0xEAAB48 - 0x10` [V]. The pointer at 0xEAAB48 addresses the CommonRenderDevice part of the object.

Everything the task called "device+X" (ring, CS, semaphores) lives in **PCDX11DeviceManager**, not in PCDX11RenderDevice.

---

## 1. Async create item: `cdc::PCDX11AsyncCreateResource`

### 1.1 Base layout (0xC bytes) [V]

| off | type | meaning |
|---|---|---|
| +0x0 | vtbl* | **one slot only**: `[0] asyncCreate(uint32 arg)`, thiscall (ecx=item, 1 stack arg, `ret 4`). The abstract base vtable is 0xA971B0. |
| +0x4 | uint32 | queue index. It is valid only if `< 0x400` **and** `ring[idx].item == this`. The worker sets it to 0x400 under the CS when it takes the item. **No constructor initializes it.** It is garbage until the first enqueue. |
| +0x8 | uint8 | pending ("keepWaiting"). `request` sets it to 1. The worker clears it after `asyncCreate` returns, without the lock. |
| +0x9 | uint8 | requested. `request` sets it to 1 and nothing clears it. This makes `request` idempotent. |

- There is **no destroy slot** in this vtable.
- Destruction goes through the owning class destructor, which calls `cancelOrAwait` (0x54C850, section 3.4).
- D3D objects are released by the PCDX11InternalResource side: `vtbl[2]` `internalRelease`, for example 0x549CD0, which calls `Release()` on the COM pointer at subobject+0xC directly.

### 1.2 Companion base `cdc::PCDX11InternalResource` (0xC bytes) [V]

| off | meaning |
|---|---|
| +0x0 | vtbl. `[0]` deleting dtor, `[1]` internalCreate, `[2]` internalRelease, `[3]` (bool, called by the device-reset walk), `[4]`,`[5]` nullsub 0x9A2B10 |
| +0x4 | prev in the deviceManager list (deviceManager+0x4 head, +0x8 tail). Unlinked by 0x549E80, which sets it to 0xFFFFFFFF. |
| +0x8 | next |

- The ctor is 0x54C760. It appends to the deviceManager list via 0x575440, **without a lock**.
- `0x54C7B0 internalCreateIfAllowed`: `if (!deviceManager->byte[0x30]) jmp vtbl[1]`.

### 1.3 Subclasses

| class | size | async item at | async vtbl, [0] create vfunc | what the create does |
|---|---|---|---|---|
| PCDX11PixelShader | 0x3C | +0x0 | 0xA98B6C, **0x54C8A0** | `dev->CreatePixelShader(bin.bytecode, (uint16)bin.size, NULL, &this->d3d)` (ID3D11Device vtbl +0x3C). arg ignored. |
| PCDX11VertexShader | 0x30 | +0x0 | 0xA98EB4, **0x553600** | `CreateVertexShader` (vtbl +0x30), same shape |
| PCDX11BitmapTexture | ? | +0x30 (pending at +0x38) | 0xA9714C, **0x52FF00** | `CreateTexture2D` (+0x14) or `CreateTexture3D` (+0x18). arg = initial data pointer or 0. |
| PCDX11Texture (BitmapTexture at +4) | ? | +0x34 (pending at +0x3C) | 0xA98238, **0x547620** | calls 0x52FF00, then frees the staging buffers at +0x114/+0x120/+0x124 |
| PCDX11ProceduralTexture | ? | +0x34 | 0xA982A0 | not traced |
| PCDX11StaticConstantBuffer | ? | +0x14 | 0xA971D0, **0x5307F0** | `CreateBuffer` (+0xC) |

- Hull, Domain and Compute shaders are **not** AsyncCreateResource. They are created synchronously in `internalCreate` on the constructing thread (CreateHullShader 0x54C730, CreateDomainShader 0x54C4F0, CreateComputeShader 0x549CF0).
- The vtable slot numbers above use the D3D11 order: +0x0C CreateBuffer, +0x14 Texture2D, +0x18 Texture3D, +0x30 VS, +0x3C PS.

### 1.4 PixelShader and VertexShader layout [V]

```
+0x00 AsyncCreateResource (vtbl, idx, pending, requested)
+0x0C InternalResource    (vtbl PS 0xA98B50 / VS 0xA98E98, prev, next)
+0x18 ID3D11PixelShader* / ID3D11VertexShader*   (0 until created)
+0x1C ShaderBinary: +0x1C DXBC wrapper*, +0x20 bytecode* (= wrapper+16), +0x24 ownsData u8, +0x25 u8 [?], +0x26 uint16 bytecode size
+0x28 refCount (ShaderManager dedup, ctor sets 1)
+0x2C hashNext (ShaderManager bucket chain)
+0x30 (PS only) vector {size, cap, data}, freed in dtor, purpose unknown [?]
```

- The wrapper header is 16 bytes: `+0x4` size and `+0x8/+0xC` a 64-bit hash. The hash is the dedup key and the ShaderUsage key.
- **Why PS are lazy:**
  - PS InternalResource `vtbl[1]` (internalCreate) is 0x54C890, which is `mov al,1; ret`. It never requests creation.
  - VS `vtbl[1]` is 0x553690, which does `ecx -= 0xC; d3d = 0; request(0)`. A VS is therefore queued the moment it is constructed (constructor 0x553590 calls 0x54C7B0).
- **Dedup:** the ShaderManager (0x54DF50 for PS, 0x54E040 for VS) looks the hash up in its buckets and bumps refCount when it finds a match. One PixelShader object can be shared by several tables and libraries.

---

## 2. Device manager: queue and worker

### 2.1 Fields (offsets from `deviceManager`) [V]

| off | meaning |
|---|---|
| +0x24 | ID3D11Device* |
| +0x2C | ID3D11DeviceContext* (immediate) |
| +0x30 | u8. Set while 0x549F10 walks the InternalResource list (device reset). While it is set, 0x54C7B0 does not create. |
| +0x34 / +0xBC | DisplayConfig (0x88 bytes each). `vtbl[1]` (+0x04) returns the active one. Fields: +0x18 enableVsync, +0x19 enableTripleBuffer, +0x24 dontDeferShaderCreation, +0x25 useShaderUsageData. |
| +0x14C | PCDX11ShaderManager* |
| +0x150 | PCDX11StateManager* (0x600 bytes, created by 0x549ED0) |
| **+0x180** | creation ring: 0x400 slots of `{item*, uint32 arg}`, **8 bytes each**, ending at +0x2180 |
| **+0x2180** | queue 2 ring: 0x2000 slots of `IUnknown*`, 4 bytes each, ending at +0xA180 |
| +0xA180 | worker thread HANDLE |
| **+0xA184** | CRITICAL_SECTION (24 bytes, spin count 0x200) |
| +0xA19C | enqueue counter (LONG, InterlockedIncrement). Producer slot = `(counter_after - 1) & 0x3FF`. |
| +0xA1A0 | dequeue index (worker only) |
| **+0xA1A4** | free-slot semaphore (init 0x400, max 0x400) |
| +0xA1A8 | filled semaphore (init 0, max 0x400) |
| +0xA1AC / +0xA1B0 | queue 2 enqueue counter / dequeue index (`& 0x1FFF`) |
| +0xA1B4 / +0xA1B8 | queue 2 free (0x2000) / filled semaphores |

- The ring has no explicit head or tail fields. The head is the interlocked counter (+0xA19C) and the tail is the worker index (+0xA1A0). The semaphores carry the counts.
- Setup is in 0x54A3A0 (fastcall ecx=deviceManager): it creates the 4 semaphores, initializes the CS with spin count 0x200, and calls `CreateThread(stack 0x4000, 0x54A100, deviceManager)`.
- Shutdown is in 0x54A440: it pushes a 0 into queue 2, joins the thread and closes the handles.

### 2.2 Enqueue `0x54A000` (thiscall ecx=deviceManager, args item, arg, `ret 8`) [V]

```
WaitForSingleObject(free, INFINITE)        // blocks the caller if 0x400 items are outstanding
i = (InterlockedIncrement(&+0xA19C) - 1) & 0x3FF
ring[i].item = item; ring[i].arg = arg      // NOT under the CS
if (item) item->queueIndex = i              // NOT under the CS, written after the slot
ReleaseSemaphore(filled, 1, NULL)
```

The only call site is 0x54C7EA (`e8 11 d8 ff ff`), inside request.

### 2.3 Cancel `0x54A060` (thiscall ecx=deviceManager, arg item, `ret 4`, returns al) [V]

```
EnterCS; i = item->queueIndex; if (i < 0x400) ring[i].item = 0; LeaveCS; return i != 0x400;
```

- It does not check `ring[i].item == item`.
- It returns true ("removed, do not wait") for any index other than 0x400.

### 2.4 Worker `0x54A100` (thread proc, stdcall arg=deviceManager) [V]

```
loop:
  r = WaitForMultipleObjects(2, {filled(+0xA1A8), relFilled(+0xA1B8)}, FALSE, INFINITE)   // site 0x54A12C
  if r == 0:                                   // creation queue has priority
     EnterCS(+0xA184)
       idx  = +0xA1A0
       item = ring[idx].item
       if (item) item->queueIndex = 0x400     // claim, under the lock
     LeaveCS
     if (item) { item->vtbl[0](item /*ecx*/, ring[idx].arg /*read after LeaveCS*/); item->pending = 0; }   // +8 cleared OUTSIDE the lock, after create
     +0xA1A0 = (idx + 1) & 0x3FF
     ReleaseSemaphore(free(+0xA1A4), 1)
     release-burst counter = 0
  else:                                        // queue 2: deferred COM Release
     p = relRing[+0xA1B0]; if (!p) return 0     // shutdown sentinel
     p->lpVtbl[2](p)  (IUnknown::Release, stdcall)
     +0xA1B0 = (+0xA1B0 + 1) & 0x1FFF; ReleaseSemaphore(relFree, 1)
     after 51 consecutive releases: Sleep(1)   // push at 0x54A1ED
```

Items are processed strictly in FIFO order, one at a time. A cancelled slot (item 0) is skipped and still frees its semaphore count.

**Safe inline create protocol** (what `binary/patch/cave.s` does, and what a hook on 0x54C800 should do):

```
if (!item->pending) return;
EnterCS(&dm->cs)
  i = item->queueIndex
  if (i < 0x400 && dm->createRing[i].item == item) {   // still queued, not yet claimed
      arg = dm->createRing[i].arg; dm->createRing[i].item = 0; item->queueIndex = 0x400;
      LeaveCS; item->vtbl[0](item, arg); item->pending = 0; return;
  }
LeaveCS
while (item->pending) wait;                             // worker owns it; it clears pending
```

Under the CS this is race-free against the worker claim.

Remaining windows [V by reasoning]:
- **Enqueue writes the slot before queueIndex.** When the request comes from another thread, an awaiter can see a stale or garbage index. The `slot == item` check makes that fall back to waiting, which is safe.
- **The ID3D11Device is free-threaded.** D3D11CreateDevice gets the SINGLETHREADED flag only if byte 0xEAAB78 != 0. That byte is BSS and is never written by direct reference, so the flag is never set.
- **The create vfuncs only call ID3D11Device methods**, never the immediate context. Calling them on the render thread is therefore legal.

---

## 3. Request and await functions

All of these are thiscall with ecx=item. The original bytes are in `engine.h`.

### 3.1 `request` 0x54C7D0 (1 stack arg `arg`, `ret 4`) [V]

```
if (!item->requested) { item->requested = 1; item->pending = 1; deviceManager->enqueue(item, arg); }
```

- Body: 34 bytes, 0x54C7D0..0x54C7F1, followed by int3 padding to 0x54C800.
- Prologue: `80 79 09 00 | 75 19 | b0 01 | 88 41 09`. A 5-byte jmp covers 6 bytes, including a `jne rel8`. **Replace the whole function. Do not trampoline it.**

Callers:

| call site | caller |
|---|---|
| 0x53037C | texture upload 0x530290, arg = data pointer |
| 0x5307DC | static CB 0x5307D0 |
| 0x54C8E6 | precreate 0x54C8D0 |
| 0x5535F9 | PS requestShader 0x5535F0 |
| 0x55369C | VS internalCreate 0x553690 |

### 3.2 `awaitResource` 0x54C800 (no args, plain `ret`) [V]

```
push esi; mov esi,ecx; if (byte[esi+8]) { do Sleep(1) while (byte[esi+8]); }   // push 1 at 0x54C811
```

- Body: 31 bytes, `56 8b f1 8a 46 08 84 c0 74 13 57 8b 3d a0 12 a8 00 6a 01 ff d7 8a 4e 08 84 c9 75 f5 5f 5e c3`.
- The first 6 bytes (`56 | 8b f1 | 8a 46 08`) have no relative operand.
- All 11 callers test `pending` inline before calling, so a hook only sees items that are pending. Sleep comes from IAT 0xA812A0.

| call site | function | object |
|---|---|---|
| 0x53040D | 0x530400 BitmapTexture::get (vtbl[0]) | tex+0x30 |
| 0x53086D | 0x530860 StaticCB get | cb+0x14 |
| 0x53B73A | 0x53B5D0 draw (PS from table, requests via 0x5535F0 at 0x53B72A) | PS |
| 0x53CA14, 0x53CA76, 0x53CAD8, 0x53CB4C, 0x53CBAF | 0x53C370 RenderDevice ctor (main thread, builtin textures) | tex+0x34 |
| 0x547C1F | 0x547B50 BitmapTexture ctor (synchronous create on the constructing thread) | tex+0x30 |
| 0x54F8BA | 0x54F890 StateManager::setPixelShader | PS |
| 0x54F9EA | 0x54F9C0 StateManager::setVertexShader | VS |

### 3.3 Bind wrappers 0x54F890 / 0x54F9C0 (thiscall ecx=StateManager, arg shader, `ret 4`) [V]

```
if (s != sm->current[PS +0x198 | VS +0x19C]) {
  if (s) { if (!s->requested) 0x5535F0(s); if (s->pending) await(s);
           sm->ctx(+0x10)->PSSetShader(+0x24) / VSSetShader(+0x2C)(s->d3d, NULL, 0); current = s; }
  else   { ctx->XSSetShader(NULL,NULL,0); current = 0; }
}
```

- 0x5535F0 is a thiscall with ecx=shader and no args: `shader->d3d = 0; request(0)`. It is shared by PS and VS (identical code folding).
- The first 5 bytes of each wrapper are `56 | 8b 74 24 08`, with no relative operand.

| wrapper | call sites | in functions |
|---|---|---|
| 0x54F890 | 0x5337CB, 0x5339CF, 0x53711E, 0x5372D4, 0x53785B, 0x53787D, 0x537BEB, 0x537FF2, 0x539CA4, 0x53B3FE, 0x53B52D, 0x53B7AB, 0x53B855, 0x53B934 | 0x533730, 0x533940, 0x536F80, 0x537240, 0x5376F0, 0x5379E0, 0x537D30, 0x539C10, 0x53AF80, 0x53B5D0 |
| 0x54F9C0 | 0x5337AB, 0x5339B1, 0x537138, 0x5372F0, 0x5378AD, 0x537C4C, 0x538022, 0x539C8D, 0x53B322, 0x53B75A, 0x5410CF | 0x533730, 0x533940, 0x536F80, 0x537240, 0x5376F0, 0x5379E0, 0x537D30, 0x539C10, 0x53AF80, 0x53B5D0, 0x541090 |

### 3.4 `cancelOrAwait` 0x54C850 (no args, plain `ret`) [V]

```
if (item->requested) { if (!deviceManager->cancel(item) /*0x54A060*/) while (item->pending) Sleep(1); }   // push 1 at 0x54C877
```

- Body: 0x35 bytes. The first 7 bytes (`56 | 8b f1 | 80 7e 09 00`) have no relative operand.
- Callers: destructors only.

| call site | destructor |
|---|---|
| 0x530792 | StaticCB 0x530780 |
| 0x5309E2 | StaticCB 0x5309D0 |
| 0x547951 | ~PCDX11Texture 0x547920 |
| 0x54C923 | ~PCDX11PixelShader 0x54C910 |
| 0x553653 | ~PCDX11VertexShader 0x553640 |

**Destroying an object that is still queued is safe:** its slot is nulled. If the worker already claimed it, the destructor waits until creation finishes.

### 3.5 Enqueue and cancel prologues

| function | first bytes | patch note |
|---|---|---|
| enqueue 0x54A000 | `56 8b f1 8b 86 a4 a1 00 00 6a ff 50 ff 15 84 12` | 5-byte patch spans 9 bytes, no relative operand |
| cancel 0x54A060 | `53 56 57 8b f9 8d 9f 84 a1 00 00` | 5 bytes |

---

## 4. Shader libraries, tables, precreate

### 4.1 Objects [V/R]

**PCDX11ShaderLib** (0x18 bytes):

| off | meaning |
|---|---|
| +0x0 | IShaderLib vtbl 0xA980E4: `[0]` dtor 0x546E40, `[1]` fill 0x547020 |
| +0x4 | RenderExternalResource vtbl 0xA980DC |
| +0x8 | link [?] |
| +0xC | renderDevice |
| +0x10 | table* |
| +0x14 | streaming buffer |

- Ctor 0x546EA0 `(blob, type, renderDevice)`. Type 1 is PS: it builds a table (0x54EE20) and then calls **0x54F000** at **0x546F0E**.
- Streamed path `fill` 0x547020: once the last chunk arrives, `buffer[0]` selects the type. Type 1 is PS: it builds the table and calls **0x54F080** at **0x5470B6**.

**PCDX11PixelShaderTable** (0x20 bytes, vtbl 0xA98C60):

| off | meaning |
|---|---|
| +0x4 | blob |
| +0x8 | offsets (= blob+8; 0xFFFFFFFF = empty) |
| +0xC | count (= blob[0] >> 2) |
| +0x14 | `PixelShader*[count]` (0 for empty slots) |
| +0x18 | ownsBlob |
| +0x1C | precreate state (0 none, 1 all, 2 gated, 3 gated+variant) |

- The ctor 0x54EE20 `(blob, takeCopy, flag3)`, `ret 0xC`.
- If takeCopy: blob dword at each offset ^= (i & 1).
- It then creates each shader via `ShaderManager::createPixelShader` 0x54E570 (thiscall ecx=deviceManager+0x14C, `(blob, takeCopy, 1)`).
- If flag3 == 0 and `config.dontDeferShaderCreation`, it registers the table in `renderDevice+0x111310`. That vector is only ever removed from (0x539D70) or freed. Nothing consumes it.

**VS table:** vtbl 0xA98C58, 0x1C bytes, ctor 0x54ECE0, dtor 0x54EDD0. Each VS self-requests at construction.

### 4.2 Iteration: `0x54F000` and `0x54F080` (thiscall ecx=PixelShaderTable, no args, plain `ret`) [V]

```
sm = deviceManager->stateManager            // +0x150
table->state = 2 (0x54F000) or 3 (0x54F080)
for i in 0..table->count-1:
   s = table->shaders[i]
   if (s && s->d3d == 0
       && (0x54F080 only: (i & 3) == lightVariantA || (i & 3) == lightVariantB)   // from renderDevice->[+0x94]-0xC +0xE1C/+0xE20
       && usageLookup(sm, s->bin.wrapper->hashLo, hashHi))                      // 0x54EA00
   {  deviceManager->vtbl[7]();   // +0x1C, result ignored [?]
      vectorPushPtr(&sm->pendingPS /*+0x5EC*/, &s);  }   // 0x54EC40 (thiscall ecx=vector, arg &value, ret 4; grows x2)
```

`0x54EFA0` is the same loop with **no gate** (state 1).

### 4.3 StateManager fields [V]

- **+0x5CC** `vector<uint64 hash>`: `{size +0x5CC, capacity +0x5D0, data +0x5D4}`. `data` is 7 when empty.
  - Filled by 0x551530 (thiscall, arg = 0-terminated uint64 list): it sets size = 0, then pushes via 0x6B0DD0.
  - 0x54EA00 is a linear search `(lo, hi)`, thiscall `ret 8`, returns al.
- **+0x5EC** `vector<PixelShader*>`: `{size +0x5EC, capacity +0x5F0, data +0x5F4}`.
  - Pushed only at 0x54EFE2 (0x54EFA0), 0x54F058 (0x54F000) and 0x54F141 (0x54F080).
  - Popped only at 0x53A44C. The StateManager ctor (0x551A7C) initializes it and the dtor (0x5515CC) frees it.
- Other fields: +0xC render thread id (written each frame), +0x10 ID3D11DeviceContext*, +0x198 current PS, +0x19C current VS.

### 4.4 Startup in the shipped game [V]

1. `FUN_006184F0` sees DX11. Near 0x6185B6 / 0x618684 it probes `ShaderUsageDX11.bin`, which does not exist, so it calls `0x539850(renderDevice, 0)`.
2. That runs `0x551530(sm, NULL)`, which leaves the usage list empty.
3. It then runs `0x54F000` on the 5 **builtin** PS tables embedded in the renderDevice (+0x10D70, +0x10DB0, +0x10DD0, +0x10DF0, +0x10D90). The gate is false, so they push nothing.
4. Then `0x5316B0` runs:
   - `0x546DC0` (lib->table, then **0x54EFA0, ungated**) on 6 builtin libs: renderDevice +0x10BF8, +0x10BFC, +0x10C08, +0x10C14, +0x10C0C, +0x10C10.
   - `0x546DB0` (0x54F080, gated) on +0x10BE4, +0x10C18, +0x10C1C.

**So the precreate machinery is live in the shipped game, but only for builtin libraries.** Every PS in those 6 libs is pushed into `+0x5EC` and drained by the render thread. Per-level libraries loaded later go through the gated paths and push nothing.

### 4.5 Drain (render thread, inside the scene loop of FUN_0053A170) [V]

At 0x53A383..0x53A476, once per scene per frame:

```
if (sm->pendingPS.size && renderDevice[+0x111358] /*set 1 in ctor, never cleared*/) {
  t0 = QPC; n = 0
  while (size && n < 50 && elapsed < 0.1 s /*double at 0xA89800*/) {
     s = data[size-1]; size--;            // LIFO pop
     0x54C8D0(s): g[0x12AB963]=1; if (!s->d3d) { s->d3d=0; request(s,0); } g[0x12AB963]=0   // only enqueues; no await
     n++ } }
```

The byte at 0x12AB963 is never read.

### 4.6 Unload path and lifetime [V]

1. `ShaderLib` release is deferred.
   - The resource is pushed onto renderDevice's deferred list (+0x1E8 old head, +0x1EC new head, +0x1F0 tail, two generations).
   - It is freed by **0x538BA0**, called from **0x538D20** each frame (believed to be the main thread BeginFrame [?]) and from 0x55F930. The final flush is in the RenderDevice dtor 0x53BB20.
2. `~ShaderLib` 0x546E40 calls `table->vtbl[0](1)`.
3. `~PixelShaderTable` 0x54EF40:
   - Removes the table from `renderDevice+0x111310` (0x539D70). That is bookkeeping only.
   - Calls `0x54E870`: for each non-null slot, `ShaderManager::releasePixelShader` 0x54E5C0, which calls `0x54E130`.
   - Then frees the slot array.
4. `0x54E130` (thiscall, ecx=ShaderManager unused, args `(shader, buckets)`, `ret 8`) decrements refCount. At 0 it unlinks the shader from its bucket and calls `internal.vtbl[0](1)`, which is 0x54C900, then 0x54C910.
5. `~PixelShader` 0x54C910 (**scalar deleting dtor**: thiscall ecx=this, 1 stack arg `flags`, `ret 4`, returns `this` in eax; bit 0 of flags frees the object; 0x54C900 is the thunk from the InternalResource subobject, `sub ecx,0xc; jmp 0x54C910`) [V]:
   - `cancelOrAwait` (0x54C850). This is queue safe: it nulls the slot or waits.
   - InternalResource `vtbl[2]` releases the ID3D11PixelShader.
   - Frees the +0x30 vector and the ShaderBinary.
   - Unlinks from the deviceManager list (0x54C790), then frees.

**Nothing removes a dying shader from `StateManager+0x5EC`.** If a PS still sits in the pending vector when its refcount hits 0, the render thread later calls 0x54C8D0 on freed memory and the worker calls through a freed vtable. That is a use-after-free.

- In the shipped game this cannot happen: only builtin libs feed the vector, and they live until device shutdown.
- **It becomes reachable if per-level libs are fed into the vector** (forcing the 0x54EA00 gate true, or a DLL precreate list).
- Any DLL precreate list must hold a reference or remove entries. Usable hook points:
  - 0x54C910 (PS scalar deleting dtor, thiscall(this, flags), `ret 4`, returns this; 7-byte prologue, no relative operand).
  - 0x54E130 (refcount drop; prologue `83 ec 08 8b 54 24 0c`).
  - 0x54EF40 (table dtor, before shaders die; prologue `56 8b f1 c7 06 60 8c a9 00`).
- Taking a reference by incrementing +0x28 is possible, but then the DLL must release through 0x54E5C0 itself [?].

---

## 5. Frame pacing [V]

- **SetMaximumFrameLatency**, in FUN_0054A930 (device creation):
  - QI at 0x54AB74 for IID 77db970f-6276-48ba-ba28-070143b4392c (IDXGIDevice1, at VA 0xA9881C).
  - Then at VA **0x54AB80** (file 0x149F80) `8b 51 30 | 6a 01 | 50 | ff d2`.
  - The immediate byte to patch is at **VA 0x54AB84** (RVA 0x14AB84, file 0x149F84), value 0x01.
- **Swap chain**, FUN_0054D680 (thiscall ecx=RenderContext):
  - BufferCount is computed at **0x54D71E** `38 43 19` (cmp cfg+0x19 enableTripleBuffer, al), then 0x54D726 `0f 95 c0` setne, then 0x54D731 `83 c0 02` add eax,2. So BufferCount = 2 + (tripleBuffer != 0).
  - The value is stored in the DXGI_SWAP_CHAIN_DESC and in RenderContext+0x28.
  - Other desc fields: SwapEffect 0 (DISCARD), Flags 2 (ALLOW_MODE_SWITCH).
  - Swap chain creation goes through `IDXGIFactory::CreateSwapChain` (vtbl +0x28), then `MakeWindowAssociation(hwnd, 1)`.
- **Present**, FUN_0054C9F0 (thiscall ecx=RenderContext, `ret 0xC`):
  - SyncInterval = (cfg.enableVsync != 0) OR (deviceManager+0xB4 != 0). The meaning of +0xB4 is unknown [?], possibly fullscreen state.
  - Flags = 0.
  - The call is at **0x54CA2C** `ff d2`, with the swap chain at RenderContext+0x34.
- There is no frame limiter in EndFrame 0x53C270.

---

## 6. Thread model [V unless noted]

| thread | entry | touches |
|---|---|---|
| **Main** | game loop | Resource loading: ShaderLib ctor/fill, VS self-request, texture `request` (0x530290); `BitmapTexture` ctor (sync await 0x547C1F); RenderDevice ctor awaits (0x53CA14..0x53CBAF); builtin precreate 0x539850; EndFrame 0x53C270 (signals render thread, waits up to 500 ms); deferred releases 0x538BA0 via 0x538D20 [?]; DF per frame 0x628490 (called from 0x618BA7) [? thread] |
| **Render** | 0x53BAD0, created in 0x53C370 at 0x53CC98 (`CreateThread(stack 0x20000)`) | loops `WaitForSingleObject(renderDevice+0x111598)` then `FUN_0053A170(0)`. All draws: StateManager set PS/VS (request + await), texture get (await), pending drain. Writes its thread id to renderDevice+0x11A8 and StateManager+0xC. |
| **Async create worker** | 0x54A100 | All AsyncCreateResource creates (PS, VS, BitmapTexture/Texture, StaticCB) and the deferred COM Release queue |
| Others | 0x4A2E40, 0x4A1210, 0x4522A0, 0x5B1550, 0x6AE3B0, 0x6AE690, 0xA09D40 (CreateThread sites) | IO, filesystem, sound and similar; not traced |

- If byte 0xEAAB78 is set, EndFrame runs `FUN_0053A170` on the main thread (single-threaded render). That byte is never set (BSS, no writer), so the render thread is always used.
- The main thread id is at renderDevice+0x11AC.

**ID3D11Device create calls outside the worker.** Found by a heuristic scan of every `deviceManager->+0x24` vtable call; it may miss calls through cached device pointers.
- CreateBuffer: 0x5308B0, 0x538850, 0x5471A0, 0x547420
- CreateTexture3D: 0x544C50
- CreateRenderTargetView: 0x5308B0, 0x5471A0, 0x54C350, 0x54D680, 0x556770
- Hull/Domain/Compute shaders: section 1.3

**CreatePixelShader and CreateVertexShader are called only from 0x54C8A0 and 0x553600, the worker vfuncs.** The device is free-threaded, so running those vfuncs on another thread is legal.

---

## 7. DFEngine interface

- The exe imports `DfEngine.dll!GetDFEngine` by name (hint 0). The IAT slot is 0xA81040 and the only call is `ff 15 40 10 a8 00` at **0x628292**.
- **GetDFEngine:** no args, plain `ret` (cdecl and stdcall without args are identical). It returns `IDFEngine*`. The original returns a static singleton (DFEngine_orig.dll VA 0x100CACD8, vtable 0x100A6894, 24 slots).
- **NULL is fully supported by the exe.** FUN_00628210 stores 0 in the manager, sets `initDone`, returns false and never retries. Every user of the engine pointer at manager+4 null-checks it first: 0x628490, 0x628130, 0x628420, 0x628060, 0x6EFF40, 0x6F00B0. The `+0x10` helper object is created only after a successful Start.
- **A stub `GetDFEngine` that returns NULL is therefore a valid, complete replacement.**

Methods are COM style: `__stdcall`, explicit `this`, callee cleans. Arg counts were checked against the `ret N` of each slot in DFEngine_orig.dll.

**Call sites push extra (name, 0) arguments.** Many call sites push `("<MethodName>", 0)` before the real args. Those two words are **not** method arguments. After the call, the exe pushes eax and calls the logger 0x9A2B10 (a `ret` nullsub), then does `add esp,0xC`.

| slot (+off) | ret | exe usage |
|---|---|---|
| 1 (+0x04) Start | 0xC | `Start(this, 0x04050000, &params)` at 0x628362. 0 = success. Nonzero: engine pointer zeroed. params holds the alloc/free callbacks 0x4D4430/0x5FB300 and the paths from SHGetFolderPath/GetTempPath. |
| 2 (+0x08) Release | 4 | shutdown 0x628479 |
| 4 (+0x10) CreateObjectInstance | 0x14 | `(this, a, b, desc*, &handle)` at 0x6F014E |
| 5 (+0x14) DestroyObjectInstance | 8 | `(this, handle)` at 0x6EFF6C |
| 6 (+0x18) DestroyAllDfObjects | 8 | `(this, 0)` at 0x628465 |
| 7 (+0x1C) RefreshCurrentCreatives | 8 | `(this, zoneName)` at 0x62850C and in 0x628130 |
| 8 (+0x20) Update | 4 | per frame at 0x6284C7 (unless state==1) and in 0x628060 |
| 9 (+0x24) StartZone | 8 | `(this, zoneName)` at 0x6283D6 and in 0x628130 |
| 10 (+0x28) EndZone | 8 | `(this, oldZoneName)` in 0x628130 |
| 14 (+0x38), 15 (+0x3C) | 8 each | camera matrices in 0x628060 [? names] |
| 18 (+0x48) | 8 | `(this, 1)` at 0x6284DA and 0x62844E; `(this, 0)` in 0x628060 [? meaning] |

- Slots 0, 3, 11 to 13, 16, 17 and 19 to 23 are never called by the exe.
- A non-NULL stub must provide the full 24-slot vtable with the correct `ret` sizes (`engine.h` `IDFEngineVtbl`). Start must return 0 and the other int methods should return 0.
- **DF manager singleton** at 0x1C78C84 (accessor 0x628AA0): +0x4 engine, +0x8 zone (init -1), +0x10 helper, +0x14 float, +0x18 state (init 2), +0x1C initDone.
  - Init 0x628210 is called from 0x63403A and 0x6340BF, guarded by global 0x1C7BF63.
  - Per frame: 0x628490 from 0x618BA7.
  - Zone change: 0x628130 from 0x61E406.
  - Shutdown: 0x628420 from 0x632F0E and atexit 0xA761F0.

---

## 8. Hook target summary (VA / RVA / first bytes)

| VA | RVA | conv | first bytes | 5-byte jmp span |
|---|---|---|---|---|
| 0x54C7D0 request | 0x14C7D0 | thiscall, 1 arg, ret 4 | 80 79 09 00 75 19 b0 01 | 6, includes jne rel8: replace whole fn |
| 0x54C800 await | 0x14C800 | thiscall, 0 args | 56 8b f1 8a 46 08 84 c0 | 6, clean |
| 0x54C850 cancelOrAwait | 0x14C850 | thiscall, 0 args | 56 8b f1 80 7e 09 00 74 | 7, clean |
| 0x54A000 enqueue | 0x14A000 | thiscall ecx=dm, 2 args, ret 8 | 56 8b f1 8b 86 a4 a1 00 00 | 9, clean |
| 0x54A060 cancel | 0x14A060 | thiscall ecx=dm, 1 arg, ret 4 | 53 56 57 8b f9 | 5, clean |
| 0x54A100 worker | 0x14A100 | stdcall thread proc | 83 ec 08 53 55 | 5, clean |
| 0x54C8A0 PS create | 0x14C8A0 | thiscall, 1 arg, ret 4 | a1 40 b9 2a 01 | 5, absolute (relocated) operand |
| 0x553600 VS create | 0x153600 | thiscall, 1 arg, ret 4 | a1 40 b9 2a 01 | 5, absolute operand |
| 0x5535F0 PS/VS requestShader | 0x1535F0 | thiscall, 0 args | 6a 00 c7 41 18 00 00 00 00 | 9, clean (call rel32 follows) |
| 0x54C8D0 precreateOne | 0x14C8D0 | thiscall, 0 args | 83 79 18 00 c6 05 63 b9 2a 01 01 | 11, absolute operand |
| 0x54F890 setPS | 0x14F890 | thiscall ecx=sm, 1 arg, ret 4 | 56 8b 74 24 08 | 5, clean |
| 0x54F9C0 setVS | 0x14F9C0 | thiscall ecx=sm, 1 arg, ret 4 | 56 8b 74 24 08 | 5, clean |
| 0x54C910 ~PixelShader | 0x14C910 | thiscall, 1 arg (flags), ret 4, returns this | 56 8b f1 57 8d 7e 0c | 7, clean |
| 0x54E130 shaderUnref | 0x14E130 | thiscall, 2 args (shader, buckets), ret 8 | 83 ec 08 8b 54 24 0c | 7, clean |
| 0x54EF40 ~PixelShaderTable | 0x14EF40 | thiscall, 1 arg, ret 4 | 56 8b f1 c7 06 60 8c a9 00 | 9, absolute operand |
| 0x54F000 queueUsed | 0x14F000 | thiscall ecx=table | 51 a1 40 b9 2a 01 | 6, absolute operand |
| 0x54F080 queueUsed4 | 0x14F080 | thiscall ecx=table | 83 ec 0c a1 40 b9 2a 01 | 8, absolute operand |
| 0x54EA00 usageLookup | 0x14EA00 | thiscall ecx=sm, 2 args, ret 8 | 8b 81 d4 05 00 00 | 6, clean |
| 0x53A170 frame | 0x13A170 | thiscall ecx=rd, 1 arg, ret 4 | 55 8b ec 83 e4 f0 | 6, clean |
| 0x628490 DF per frame | 0x228490 | thiscall ecx=dfm | 55 8b ec 83 e4 f0 | 6, clean |
| 0x54AB84 latency imm | 0x14AB84 | data byte | 01 (in 6a 01) | n/a |
| 0x54C811 / 0x54C877 / 0x54A1ED Sleep imm sites | | | 6a 01 | n/a |

- Absolute operands are already relocated in memory, so a trampoline that copies the live bytes keeps them valid.
- Patch only after verifying the live bytes match the table above.

---

## Open issues

1. **PCDX11BitmapTexture:** total size and the fields after +0x128 were not mapped. Only the async item (+0x30/+0x34) and the +0x128 resource pointer are confirmed.
2. **Unknown deviceManager fields:**
   - The meaning of `vtbl[7]` (+0x1C), called before each pending push, is unknown.
   - The meaning of deviceManager+0xB4 (forces vsync on Present) is unknown.
3. **Deferred-release thread:** that 0x538D20 is BeginFrame on the main thread is inferred, not traced from its caller chain.
4. **PixelShader +0x30 vector:** purpose unknown.
5. **IO threads:** which thread runs `ShaderLib::fill` (0x547020) for streamed libraries is not traced (main thread or an IO thread).
6. **Create\* scan coverage:** the scan is heuristic. Calls through device pointers cached elsewhere could be missed (Texture2D in 0x52FF00 was found manually).
7. **Game folder state:** at the time of writing (2026-10-10 13:50), the game folder contains a non-stock `DFEngine.dll` (mingw build, 224506 bytes, sha256 e27b168f...) and `DFEngine_orig.dll` (the original, sha256 03032490...). These were placed by a parallel task, not by this spec task. The exe there is stock.

## DLL implementation notes (dxhrfix, src/)

- Hook B (precreate) lists pixel shaders from 0x54F000 / 0x54F080 and creates them on a DLL thread pool. The device manager teardown is 0x54C200: 0x54C040 (dm dtor, which calls shutdown 0x54A440 at 0x54C08C), then the free at 0x54C221, then the global is cleared at 0x54C229. The DLL hooks 0x54A440, so it stops the pool before the dm memory is freed.
- **Precreate stays off for the rest of the process after any dm shutdown.** The shutdown flag is never cleared. A device manager created later in the same process gets no precreate; Hook A (request, await, cancelOrAwait) keeps working.
- Table state +0x1C is 0 after the ctor 0x54EE20 (`xor ebp,ebp` then `mov [esi+0x1c],ebp` at 0x54EE7F). The DLL skips a table whose state is already 2 or 3 when 0x54F000 / 0x54F080 runs, so a table is listed once.
- **Hitch profiler (src/dxhrfix_profiler.cpp).** Frame boundary: trampoline hook on the Present wrapper 0x54C9F0 (thiscall ecx=RenderContext, 3 stack args, `ret 0xC`; only caller 0x53A584 in 0x53A170). Verified bytes `56 8b f1 8b 0d [0x12AB940] 8b 01 8b 50 04 ff d2 80 78 18 00 a1 [0x12AB940]`, prologue 9 (push esi, mov esi,ecx, mov ecx,[abs]). The first thread that calls it is the render thread. Thread identification by Win32 start address (NtQueryInformationThread class 9): render 0x53BAD0, async worker 0x54A100, ThreadedFileSystem worker 0x4A2E10 (loop 0x49FB40, ctor 0x4A0470). Other exe CreateThread entries: 0x452290, 0x4A1190, 0x5B12E0, 0x6AE390, 0x6AE670, 0xA09CB0, and the CRT `_beginthreadex` trampoline 0xA39574. ReadFile import: IAT slot 0xA81060 (callers: CRT `__read_nolock` and 0xA097C6).
- **Locks taken by the DLL** (none is held while taking another, except as noted):
  - device manager CS (dm+0xA184): Hook A request/await/cancel, precreate pool claim (pool: list CS, then dm CS).
  - precreate list CS (`g_listCs`): precreate list and batches; also taken by the totals formatter (reporter thread, profiler writer thread).
  - log CS (`g_logLock`, proxy): innermost, only around WriteFile.
  - profiler module CS (`g_modCs`): leaf. Taken by the sampler to swap its module table and by the sampler and writer around each symbol lookup. `GetModuleHandleExW` / `FreeLibrary` (loader lock) run outside it.
  - The profiler sampler takes no lock and calls nothing but SuspendThread, GetThreadContext, QueryPerformanceCounter and ResumeThread between suspending and resuming a game thread, and reads the target stack and TEB directly.
