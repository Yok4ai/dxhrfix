# Key addresses in DXHRDC.exe

All addresses are virtual addresses at the default image base 0x400000 of the Steam `DXHRDC.exe` with sha256 `8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1`. Under ASLR the proxy adds the load delta. `lists/key_functions.lst` holds the same list as input for `Decomp.java`. The full write up of each site (layouts, expected bytes, calling conventions) is in `spec/SPEC.md`, `spec/streaming.md` and `spec/vs_precreate.md`.

Decompile these first, roughly in this order.

## Async resource creation (Hook A)

| VA | name | what it is |
|---|---|---|
| 0x54C7D0 | asyncRequest | puts a resource on the async create ring. Replaced whole by dxhrfix so the slot is published under the device manager lock. |
| 0x54C800 | awaitResource | called when a thread needs a resource that is still queued; stock polls with `Sleep(1)`. Hooked: creates the resource inline. |
| 0x54C850 | cancelOrAwait | resource destructor path; removes a queued item or waits for it. Hooked. |
| 0x54A000 | asyncEnqueue | 0x400 slot ring with a free slot semaphore. |
| 0x54A100 | async create worker | the single worker thread that creates shaders, textures and constant buffers in FIFO order. |
| 0x54AB84 | frame latency byte | immediate of `push 1` before `SetMaximumFrameLatency` (inside 0x54A930). |

## Shader precreate (Hook B)

| VA | name | what it is |
|---|---|---|
| 0x54F000 | queueUsed | pixel shader table iteration when a shader library loads. Hooked. |
| 0x54F080 | queueUsed4 | second pixel shader table iteration. Hooked. |
| 0x54C910 | ~PixelShader | pixel shader destructor. Hooked so a listed shader is unlisted first. |
| 0x54A440 | dmShutdown | device manager shutdown; joins the worker. Hooked to stop the pool. |
| 0x5535F0 | psRequest | shared PS / VS requestShader. Replaced. |
| 0x54C8D0 | precreateOne | stock precreate helper. Replaced. |

## Frame loop (profiler, analysis)

| VA | name | what it is |
|---|---|---|
| 0x54C9F0 | present wrapper | `RenderContext` Present; the profiler's frame boundary. |
| 0x53A170 | renderFrame | one render frame: deferred updates, draws, Present. |
| 0x53BAD0 | render thread proc | waits for the main thread, runs renderFrame. |
| 0x53C2D7 | EndFrame wait | main thread waits INFINITE for the previous render frame (in EndFrame 0x53C270). |
| 0x53C2E5 | EndFrame wait | main thread waits up to 500 ms for the previous Present. |

## Streaming (analysis only, not patched)

| VA | name | what it is |
|---|---|---|
| 0x49FA90 | syncAll | ThreadedFileSystem: `Sleep(10)` loop until the file system queue is idle (sleep at 0x49FAAE). |
| 0x5A8320 | requestComplete | ResolveReceiver completion handler (section loading on the main thread). |
| 0x4A2E10 | FS thread start | ThreadedFileSystem thread entry (passed to `CreateThread` at 0x4A2ED4), calls the loop below. Ghidra auto analysis does not create a function here; create one in the GUI or read the instructions with `helpers/fn.sh 4a2e10 16`. |
| 0x49FB40 | FS thread loop | ThreadedFileSystem worker loop. |

## Cursor (cursor_fix)

| VA | name | what it is |
|---|---|---|
| 0x81A130 | UI update | `ShowCursor` loops and `SetCursor` every frame. |
| 0x4B07A0 | clip update | `ClipCursor` every frame. |
| 0xA813EC / 0xA813E8 / 0xA81328 | IAT slots | ShowCursor / SetCursor / ClipCursor imports, hooked by dxhrfix. |
