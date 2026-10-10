# dxhrfix: DFEngine.dll proxy for Deus Ex: Human Revolution Director's Cut

A drop-in replacement for the game's `DFEngine.dll` (the in-game ad SDK). The game loads it at startup. It forwards the ad SDK to the original DLL, renamed `DFEngine_orig.dll`, and patches the game in memory to cut render-thread stalls. Nothing on disk is patched except the swapped DLL. It works only with the stock Steam `DXHRDC.exe` (sha256 `8266b6b4...ba510a1`). Every patch site is byte-checked before it is written, and a site that does not match is skipped and logged.

## What it does

**Serialized resource requests (Hook A, request 0x54C7D0).** The game hands shader, texture and constant buffer creation to one background worker through a 1024-slot ring. The stock code publishes a slot outside the lock that the worker reads under. Under contention the worker can pick up a half-written or stale slot, which can lose a creation or run it twice. The replacement marks the resource busy and publishes the slot entirely under the game's own lock, so the worker only ever sees finished slots, in order.

**Create on demand instead of sleeping (Hook A, await 0x54C800).** When the render or main thread needs a resource that the worker has not reached yet, stock code sleeps in 1 ms steps until the worker gets to it. The hook takes the resource out of the queue and creates it right there on the waiting thread. The D3D11 device is free-threaded, so this is legal. It waits (yield, then Sleep(0), then Sleep(1)) only when the worker is already creating that exact resource.

**Safe destruction (Hook A, cancelOrAwait 0x54C850).** When a resource is destroyed while still queued, the hook removes it from the queue under the lock. Unlike stock, it checks that the slot really belongs to that resource. If the worker or the precreate pool is creating it right now, the destructor waits for that creation to finish.

**Frame latency (patch at 0x54AB84).** The game calls `SetMaximumFrameLatency(1)`. The patch lets the CPU queue `frame_latency` frames (default 2), which smooths frame pacing when the GPU or the shader compiler hiccups.

**Shader precreate (Hook B).** When a shader library is loaded (0x54F000 / 0x54F080), every pixel shader in it that nobody has asked for yet is put on a list. A small DLL thread pool creates them in the background, so DXVK compiles them before the first draw needs them. Vertex shaders are skipped because the game already queues them at load.

**Hitch profiler (diagnostics, `profiler=1`).** A sampling profiler that explains slow frames. It hooks the engine's Present wrapper (0x54C9F0) to time each frame on the render thread, samples the main, render, async create and file system threads (suspend, read the registers, scan the stack for return addresses, resume), and for every frame longer than `profiler_hitch_ms` writes a report to `dxhrfix_hitch.log`. It also counts the game's `ReadFile` calls (exe import table hook). See "Reading dxhrfix_hitch.log" below.

Safety for Hook B:
- A shader is claimed with one atomic operation, so the game and the pool never both create it.
- A shader destroyed while still listed is removed from the list first (hook on ~PixelShader 0x54C910). A shader the pool is creating makes its destructor wait.
- Two small game functions that cleared a shader pointer before requesting it (0x5535F0, 0x54C8D0) are replaced, so they can no longer wipe a shader the pool already made.
- When the device manager shuts down (0x54A440), the pool stops, the list is cleared and in-flight creations are awaited. Precreate stays off for the rest of that process.

## Install / revert

- Linux: `./install.sh [game folder]`. The default folder is `/mnt/Backup/SteamLibrary/steamapps/common/Deus Ex Human Revolution Director's Cut`.
- Windows: `install.bat "C:\...\Deus Ex Human Revolution Director's Cut"`. With no argument it uses the current folder.

The install script:
- refuses a non-stock `DXHRDC.exe`;
- copies the stock `DFEngine.dll` to `DFEngine_orig.dll` (only if that file does not exist yet, and only after checking its sha256 is `03032490...e102145e`);
- copies the proxy in;
- copies `dxhrfix.ini` only if none exists yet (an older ini without the profiler keys is fine: missing keys take the defaults);
- copies `dxhrfix_syms.tsv` (profiler symbol names), overwriting an older copy.

It prints each step and never deletes anything.

To revert, use either of these:
- `./revert.sh [game folder]` or `revert.bat`. These copy `DFEngine_orig.dll` back over `DFEngine.dll`. It is a copy, so `DFEngine_orig.dll` stays as a backup.
- Steam > Properties > Installed Files > Verify integrity. Steam restores the stock `DFEngine.dll`. `DFEngine_orig.dll`, `dxhrfix.ini` and `dxhrfix.log` are left behind, which is harmless because the stock DLL never reads them.

## dxhrfix.ini keys (section `[dxhrfix]`)

| key | default | meaning |
|---|---|---|
| hooks | 1 | 0 = patch nothing at all (stock behavior, ad SDK still forwarded, log still written) |
| precreate | 1 | Hook B shader precreate on/off (needs hooks=1) |
| pool_threads | auto | precreate threads; auto/0 = min(4, cores - 2), at least 1; max 8 |
| precreate_max_per_sec | 0 | cap on pool creations per second; 0 = unlimited |
| frame_latency | 2 | DXGI max frame latency, 1..4; 1 = stock |
| log_level | 1 | 0 install lines only; 1 also per-library precreate lines, first events and 10 s totals; 2 every event (debug, slow) |
| ad_sdk | 1 | 1 = forward to DFEngine_orig.dll; 0 = built-in stub (ads off) |
| profiler | 1 | hitch profiler on/off; independent of `hooks` (hooks=0 profiler=1 profiles the stock game); 0 = no thread, no patch |
| profiler_hz | 250 | samples per second per tracked thread, 50..4000. Under Proton one sample suspends a thread for about 0.15 to 0.2 ms: 250 Hz costs each tracked thread about 5% of its time, 1000 Hz about 15% |
| profiler_hitch_ms | 40 | a frame longer than this (ms between two Presents) is reported |
| profiler_max_dumps | 200 | reports per session |
| profiler_ring_ms | 1000 | sample history in memory, 100..4000; a longer frame is reported with its oldest part missing |
| profiler_extra_threads | 0 | 1 = also sample the game's other own threads (sound, jobs and similar) |
| profiler_readfile | 1 | count the game's ReadFile calls per thread for each report |

The profiler reads game function names from `dxhrfix_syms.tsv` next to `DFEngine.dll` (install.sh copies it; it is generated by `tools/gen_syms.py` from the Ghidra function list, the exe's RTTI vtables and the names in spec/SPEC.md). Without it, exe addresses print as `DXHRDC+0xRVA`. The environment variable `DXHRFIX_SYMS` (a Windows path) overrides the location.

## How to confirm it loaded

`dxhrfix.log` is written next to `DXHRDC.exe`. Expect these lines:

```
config: hooks=1 precreate=1 frame_latency=2 log_level=1 ad_sdk=1 pool_threads=0 precreate_max_per_sec=0
hook await VA 0x54C800 @0054C800 installed, verified 31 bytes, prologue 6
hook cancelOrAwait VA 0x54C850 @0054C850 installed, verified 53 bytes, prologue 7
replace request VA 0x54C7D0 @0054C7D0, verified 34 bytes, ...
frame latency VA 0x54AB84 @0054AB84 patched 01 -> 02 (SetMaximumFrameLatency(2))
hook ~PixelShader VA 0x54C910 ... installed
hook dmShutdown VA 0x54A440 ... installed
replace psRequest VA 0x5535F0 ... / replace precreateOne VA 0x54C8D0 ...
hook queueUsed VA 0x54F000 ... / hook queueUsed4 VA 0x54F080 ... installed
precreate: 6 Hook B patches, pool 4 threads (cores 12), max_per_sec 0
hooks: 10 patches applied (...)
precreate pool: 4 threads started (normal priority), max_per_sec 0
forwarded to DFEngine_orig.dll ...
reporter thread started (totals every 10 s)
```

Every 10 s two totals lines follow:

```
totals: requests N (lost N), inline N (late N), fallback N, await_noop N, dtor_cancel N, dtor_wait N, dtor_noop N, no_dm N, max_wait X ms, max_inline_create X ms
precreate: libs N (listed N, relisted skipped N), shaders listed N (dup skipped N), created N, lost N (already created N), skipped_unload N, cleared N, queued N, in_flight N, no_device_waits N, create_total X ms, max_create X ms
```

- `inline` counts stalls that were turned into an immediate create.
- `fallback` and `max_wait` show how long a thread still had to wait for the worker.
- `created` counts shaders the pool made ahead of time.
- Each finished library also logs a `precreate lib #N ...` line.

Any `FAILED`, `SKIPPED` or `mismatch` line means that patch was not applied, and the game runs stock for that part.

With the profiler on, these lines follow:

```
config: profiler=1 profiler_hz=250 profiler_hitch_ms=40 profiler_max_dumps=200 profiler_ring_ms=1000 profiler_extra_threads=0 profiler_readfile=1
hook present VA 0x54C9F0 @0054C9F0 installed, verified 25 bytes, prologue 9
profiler: ReadFile IAT @00A81060 patched ...
profiler: installed (2 patches), main thread N, ...
profiler: 32037 symbols loaded from ...dxhrfix_syms.tsv
profiler: started, hitch log ...dxhrfix_hitch.log, sampler tid N, writer tid N
profiler: tracking thread N as main / render / async_create / fs_worker ...
profiler: sampler running at 250 Hz, ...
profiler: hitch #N frame X ms (frame #F) dumped to dxhrfix_hitch.log, N samples
profiler: ticks N, avg tick X us, ..., frames N, max frame X ms, hitches N, dumps queued N written N dropped N, readfile N; main N: N ok 0 fail, suspended avg X us ...
```

The last line repeats every 10 s. If the Present site does not match, the log says `profiler: Present hook not installed, profiler OFF` and the game runs without it.

## Reading dxhrfix_hitch.log

One block per slow frame, starting with `==================== hitch #N: frame X ms (frame #F), <start> .. <end>, N samples`. The window is the time between the previous Present and the late one; all samples in it are aggregated. Then:

1. The `totals:` and `precreate:` counter lines (same as the 10 s lines in dxhrfix.log) and the profiler counters at that moment, to cross reference with shader activity.
2. One section per thread (render, main, async_create, fs_worker):
   - **leaf functions**: where the thread was executing (instruction pointer), grouped by function, with sample count and percent of the thread's samples in the window. A leaf in `ntdll.dll!NtWaitFor...`, `NtDelayExecution` or `RtlWaitOnAddress` means the thread was blocked, not computing.
   - **return addresses on stack**: every call site found on the stack, counted once per sample. Game call sites near the top (high percent) are the game functions the time was spent under, even when the leaf is in a system DLL.
   - **top stacks** (render and main only): the 5 most frequent distinct stacks, leaf first, then the return addresses found from the stack pointer upward.
3. **ReadFile calls overlapping the window**, per thread: calls, bytes, total and longest time spent inside ReadFile.

Notes:
- Stacks come from a conservative scan (any stack value that points just after a call instruction in a loaded module counts), so stale entries and repeated frames appear. Read them as "these call sites were on the stack", not as an exact call chain.
- Game addresses are VAs (base 0x400000, same as DXHRDC.objdump.txt) with names from dxhrfix_syms.tsv: hand names from SPEC.md, Ghidra names, `Class::vfN` (slot N of an RTTI vtable; `+k` = shared with k more vtables), or `FUN_xxxxxxxx`. A leading `?` means the address lies past the end of the named function's known body.
- Other modules print as `module!export+0xoff` (nearest export within 4 KiB) or `module+0xrva`. `d3d11.dll` and `dxgi.dll` are DXVK.

## Known risks and untested paths

- **Level load and unload are untested.** Testing ran at the main menu only (60 s runs under Proton). The paths not exercised are:
  - removing shaders from the precreate list on unload (`skipped_unload`);
  - destructor waits during a pool create;
  - the await fallback.
- **Device reset or loss** (the game's internal resource walk at 0x549F10) is not coordinated with the pool.
- **Windows ASLR is untested.** If the exe loads at a non-default base, the byte checks are rebased, but this was only run at the default base under Wine/Proton.
- **`hooks=0` was not run in the launch harness.** It is a trivial path: no hook is installed.
- **Device manager re-creation:** if the game ever re-creates the device manager in the same process, precreate stays off after the first shutdown. The other hooks keep working.
- **Process exit:** nothing is logged at process exit. The last 10 s totals line is the final record.
- **Profiler perturbation:** sampling suspends each tracked thread briefly (about 0.15 to 0.2 ms per sample under Proton). At the default 250 Hz that is about 5% of each tracked thread's time, and rare single samples took up to a few ms. It does not change what the hooks do, but frame times measured with the profiler on are slightly worse than without it. Use `profiler=0` for A/B frame time measurements.
- **revert.sh** leaves `dxhrfix_syms.tsv` and `dxhrfix_hitch.log` in the game folder; the stock DLL ignores them.
