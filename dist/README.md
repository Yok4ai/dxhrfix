# dxhrfix 0.4.0

A stutter fix for **Deus Ex: Human Revolution Director's Cut** (Steam). It replaces the game's `DFEngine.dll`, the DLL of a defunct in-game advertising SDK, with a small DLL that patches the game in memory when it starts. No game file on disk is modified except that DLL, and the original is kept as `DFEngine_orig.dll`.

Supported: the Steam `DXHRDC.exe` with sha256 `8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1`. Every patch checks the game's bytes before writing. A site that does not match is skipped and logged, and that part of the game runs stock.

Tested on Linux with Proton 11 (DXVK, NVIDIA). Windows is untested: the DLL is plain Win32 and handles ASLR, but nobody has run it there yet. Reports are welcome either way.

## Files

| file | what |
|---|---|
| `DFEngine.dll` | the fix |
| `dxhrfix.ini` | settings (copied to the game folder only if none is there yet) |
| `dxhrfix_syms.tsv` | function names for the optional hitch profiler (addresses and names only) |
| `install.sh`, `install.bat` | installers for Linux and Windows |
| `revert.sh`, `revert.bat` | put the original DLL back |

## Install

1. Extract the zip. Put the `dxhrfix-<version>` folder inside the game folder (the one with `DXHRDC.exe`), or anywhere else. Do not extract the files loose into the game folder: that would overwrite the original `DFEngine.dll` before it can be backed up.
2. Run the installer with the game not running.

Linux (Steam with Proton): open a terminal in the extracted `dxhrfix-<version>` folder and run

```
./install.sh
```

The installer finds `DXHRDC.exe` in the parent folder on its own. If you extracted the folder somewhere else, pass the game folder as the argument: `./install.sh "/path/to/Deus Ex Human Revolution Director's Cut"`. Steam > Properties > Installed Files > Browse shows where that is.

Windows: double click `install.bat` inside the extracted folder (when that folder is inside the game folder), or run `install.bat "C:\path\to\Deus Ex Human Revolution Director's Cut"` from a command prompt.

No launch options are needed. The game loads `DFEngine.dll` from its own folder.

The installer:
- checks `DXHRDC.exe`; if it is not the supported build it explains and asks before continuing;
- checks that the game's `DFEngine.dll` is the stock ad SDK DLL (sha256 `030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e`) and copies it to `DFEngine_orig.dll`, or recognizes an earlier dxhrfix and updates it; anything else stops the install;
- copies the new `DFEngine.dll` in and verifies the copy;
- copies `dxhrfix.ini` only if the game folder has none (your settings are kept on update);
- copies `dxhrfix_syms.tsv`;
- never deletes anything.

To update to a newer dxhrfix, run the new installer the same way.

## Confirm it is active

Start the game once. `dxhrfix.log` in the game folder should contain `hooks: 10 patches applied` and no `FAILED`, `SKIPPED` or `mismatch` lines. The full log format is described below under "How to confirm it loaded".

## Revert

- `./revert.sh` or `revert.bat` (same folder rules as the installer) copies `DFEngine_orig.dll` back over `DFEngine.dll`, nothing else.
- Or Steam > Properties > Installed Files > Verify integrity of game files, which restores the stock `DFEngine.dll`.

The leftover `DFEngine_orig.dll`, `dxhrfix.ini`, `dxhrfix_syms.tsv` and logs are ignored by the stock DLL and can be deleted.

## What it changes

- **Resource creation without waiting.** The engine creates every shader, texture and constant buffer on one background thread, one at a time. When the render thread needs one that is still queued, stock code sleeps in 1 ms steps until the worker gets to it (on Windows each step can take up to 15.6 ms, because the game never raises the timer resolution). dxhrfix takes the item out of the queue and creates it on the waiting thread instead; the D3D11 device is free threaded, so this is allowed.
- **Queue race fix.** Stock code publishes a queue slot outside the lock the worker reads it under, so the worker can see a half written slot. dxhrfix publishes it in order under the engine's own lock.
- **Shader precreate.** Pixel shaders are created at first draw, which on DXVK means a pipeline compile in the middle of a frame. When a shader library loads, dxhrfix creates its pixel shaders on a small thread pool ahead of time.
- **Frame latency 2.** The game hardcodes `SetMaximumFrameLatency(1)`; 2 smooths pacing when the GPU or shader compiler hiccups.
- **Cursor call dedupe.** The UI calls `ShowCursor`, `SetCursor` and `ClipCursor` every frame, and its show loop raises the cursor counter by one every visible frame. Under Wine each call is a round trip to the wineserver; one profiled 113 ms frame spent 78% of its time there. Only real changes are passed on now.
- **Ad SDK off** (`ad_sdk=0`). The ad network no longer exists and its SDK blocked the main thread on network calls. The game handles a missing ad SDK on its own.

## Settings (dxhrfix.ini, section [dxhrfix])

Every key is documented in the file itself. Changes take effect at the next game start. A missing key uses the built-in default, which equals the release default except `ad_sdk` (built-in 1).

| key | release default | meaning |
|---|---|---|
| hooks | 1 | 0 = patch nothing (stock behavior, log still written) |
| precreate | 1 | shader precreate on the thread pool (needs hooks=1) |
| pool_threads | auto | precreate threads; auto = min(4, cores - 2), at least 1; max 8 |
| precreate_max_per_sec | 0 | cap on pool creations per second; 0 = unlimited |
| frame_latency | 2 | DXGI max frame latency, 1..4; 1 = stock |
| cursor_fix | 1 | dedupe per frame cursor calls (needs hooks=1) |
| cursor_refresh_ms | 250 | pass an unchanged SetCursor / ClipCursor through at least this often |
| ad_sdk | 0 | 1 = forward the ad SDK to `DFEngine_orig.dll`; 0 = built-in stub |
| log_level | 1 | 0 install lines only; 1 also totals every 10 s; 2 every event (slow, debugging only) |
| profiler | 0 | hitch profiler for bug reports; writes `dxhrfix_hitch.log` |
| profiler_hz | 250 | samples per second per thread, 50..4000 (each sample costs about 0.2 ms of that thread's time under Proton) |
| profiler_hitch_ms | 40 | frames longer than this are reported |
| profiler_max_dumps | 200 | reports per session |
| profiler_ring_ms | 1000 | sample history kept, 100..4000 ms |
| profiler_extra_threads | 2 | 0 = core game threads only; 1 = plus other game threads; 2 = every thread including DXVK's (max 16) |
| profiler_readfile | 1 | count the game's ReadFile calls per thread in each report |

## Reporting a problem

1. Note what you were doing and roughly when the problem happened.
2. Attach `dxhrfix.log` from the game folder. It records which patches were applied and the counters every 10 s.
3. For stutter, set `profiler=1` in `dxhrfix.ini`, play until it stutters, quit, and also attach `dxhrfix_hitch.log` (and `dxhrfix.log` from the same session). Set `profiler=0` again afterwards; it costs a few percent of frame time.
4. Say whether you play on Windows or Linux (Proton version), and your GPU and driver.

Any `FAILED`, `SKIPPED` or `mismatch` line in `dxhrfix.log` means a patch was not applied; please include it.

## How to confirm it loaded

`dxhrfix.log` is written next to `DXHRDC.exe` every time the game starts. With the release `dxhrfix.ini` it begins like this (addresses and thread ids differ):

```
dxhrfix proxy 0.4.0, log ...\dxhrfix.log
exe base 00400000 (preferred 0x400000), proxy base ...
config: hooks=1 precreate=1 frame_latency=2 log_level=1 ad_sdk=0 pool_threads=0 precreate_max_per_sec=0
config: profiler=0 profiler_hz=250 profiler_hitch_ms=40 profiler_max_dumps=200 profiler_ring_ms=1000 profiler_extra_threads=2 profiler_readfile=1
config: cursor_fix=1 cursor_refresh_ms=250
site awaitResource entry VA 0x54C800 @0054C800 MATCH (want 56 8b f1 8a 46 08 84 c0, got 56 8b f1 8a 46 08 84 c0)
...
hook sites: 5/5 match
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
cursor: 3 of 3 IAT hooks installed (ShowCursor shadow count, SetCursor / ClipCursor dedupe, refresh 250 ms)
precreate pool: 4 threads started (normal priority), max_per_sec 0
ad_sdk=0: using stub engine
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

With `profiler=1`, these lines follow:

```
config: profiler=1 profiler_hz=250 profiler_hitch_ms=40 profiler_max_dumps=200 profiler_ring_ms=1000 profiler_extra_threads=2 profiler_readfile=1
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
2. One section per tracked thread (render, main, async_create, fs_worker, and with `profiler_extra_threads` the game's other threads and DXVK's):
   - **leaf functions**: where the thread was executing (instruction pointer), grouped by function, with sample count and percent of the thread's samples in the window. A leaf in `ntdll.dll!NtWaitFor...`, `NtDelayExecution` or `RtlWaitOnAddress` means the thread was blocked, not computing.
   - **return addresses on stack**: every call site found on the stack, counted once per sample. Game call sites near the top (high percent) are the game functions the time was spent under, even when the leaf is in a system DLL.
   - **top stacks** (render and main only): the 5 most frequent distinct stacks, leaf first, then the return addresses found from the stack pointer upward.
3. A `threads` summary table first (every sampled thread: samples, percent inside syscalls or unix calls, top leaf), and for each thread inside a syscall or unix call, its **native** side: the interrupted native instruction (`native leaf`, e.g. `[vdso]` = blocked in a Linux syscall, `libnvidia-glcore.so...` = running in the driver) and the files on the live native stack, printed as `file+offset` (offset into the .so file, usable with addr2line or objdump). The native registers come from the kernel signal frame Wine's suspend handler sits on; the native stack is scanned from that ESP up to Wine's syscall frame (TEB+0x218 in Proton 11). Native mappings come from `/proc/<game pid>/maps` (found through `Z:\proc`).
4. **ReadFile calls overlapping the window**, per thread: calls, bytes, total and longest time spent inside ReadFile.

Notes:
- Stacks come from a conservative scan (any stack value that points just after a call instruction in a loaded module counts), so stale entries and repeated frames appear. Read them as "these call sites were on the stack", not as an exact call chain.
- Game addresses are VAs (base 0x400000, the same addresses as in `spec/` and `tools/ghidra/ADDRESSES.md` of the source repository) with names from dxhrfix_syms.tsv: hand names from SPEC.md, Ghidra names, `Class::vfN` (slot N of an RTTI vtable; `+k` = shared with k more vtables), or `FUN_xxxxxxxx`. A leading `?` means the address lies past the end of the named function's known body.
- Other modules print as `module!export+0xoff` (nearest export within 4 KiB) or `module+0xrva`. `d3d11.dll` and `dxgi.dll` are DXVK.

## Known limits

- Windows is untested (see above).
- A device reset or loss inside the game is not coordinated with the precreate pool.
- If the game re-creates its device manager in the same process, precreate stays off afterwards; the other fixes keep working.
- Nothing is logged at process exit; the last 10 s totals line is the final record.
- The profiler suspends sampled threads briefly (about 5% of each thread's time at 250 Hz under Proton). Keep it off unless you are collecting a report.
- Remaining stutter after the fix is mostly not shader related: level streaming runs on the main thread, and DXVK can still stall on first use of some pipelines.

## License

MIT, see `LICENSE` in the source repository. No game files are included, and the DLL contains no game code.
