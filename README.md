# dxhrfix

An open source stutter fix for **Deus Ex: Human Revolution Director's Cut** (Steam). It ships as a replacement for the game's `DFEngine.dll`, the DLL of a long dead in-game advertising SDK. The game loads that DLL at startup; dxhrfix uses that moment to apply small, byte verified patches to the game in memory. Nothing on disk is changed except the swapped DLL, and the original is kept next to it.

- Supported game: only the Steam `DXHRDC.exe` with sha256 `8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1`.
- Every patch site is compared byte for byte before it is written. A site that does not match is skipped and logged, and that part of the game runs stock.
- No game files are redistributed. The DLL contains no game code: it holds its own replacement routines plus the addresses and expected bytes of the sites it patches.
- Tested on Linux with Proton 11 (DXVK, NVIDIA). **Windows is untested.** The DLL is plain Win32, handles ASLR (all addresses and expected bytes are rebased), and the installers have Windows versions, but nobody has run it on Windows yet. Reports either way are welcome.

## Results

Measured on the author's machine (Linux, Proton 11, DXVK, NVIDIA GPU, Ryzen 5 3600X), same walk through the Detroit hub, MangoHud frame time logs, 1 to 3 minutes per run. The first 20 seconds after the loading screen are excluded from the play columns, because that window is DXVK compiling pipelines on every build, stock or fixed. Stock numbers vary a lot with shader cache state, so several stock runs are listed.

| run | frames over 50 ms per minute of play | frames over 100 ms during play | worst frame during play |
|---|---|---|---|
| stock, warm cache | 4.7 | 2 | 155 ms |
| stock, cold cache | 21.9 | 8 | 680 ms |
| stock, cold cache, second run | 67.0 | 9 | 172 ms |
| stock, Steam's replayed shader cache | 48.9 | 64 | 4337 ms |
| dxhrfix 0.2 (inline create, precreate) | 3.4 | 8 | 698 ms |
| dxhrfix 0.3 (plus ad SDK off) | 0.7 | 1 | 121 ms |
| dxhrfix 0.4 (plus cursor fix) | 0.8 | 0 | 56 ms |

In the 0.4 run the single remaining frame over 50 ms was 56 ms. Everything else over 60 ms in any fixed run sat inside the first 20 seconds, where DXVK's pipeline compiler is the bottleneck. One run created about 13,000 pixel shaders on the precreate pool instead of on the render thread, and the longest time the render thread still had to wait for the worker dropped to microseconds. Your results depend on hardware, driver and shader cache state.

## Why the game stutters

Findings from disassembly of the exe (details and addresses in [`spec/`](spec/SPEC.md)):

1. **One worker creates every GPU resource.** Shaders, textures and constant buffers go through a 1024 slot FIFO to a single background thread. When the render thread needs an item the worker has not reached yet, `awaitResource` (0x54C800) polls with `Sleep(1)` until it has. The exe never calls `timeBeginPeriod`, so on Windows each poll can last up to 15.6 ms.
2. **Pixel shaders are created lazily, at first draw.** The game's option to create them up front reads `ShaderUsageDX11.bin`, a file that does not exist in the shipped game, so that path is never taken. About 14,600 pixel shaders exist; on DXVK each first use can mean a pipeline compile in the middle of a frame.
3. **A race in the queue.** The request function publishes a queue slot outside the lock the worker reads it under, so under contention the worker can see a half written slot.
4. **Per frame cursor calls.** The UI calls `ShowCursor` in a loop plus `SetCursor` and `ClipCursor` every frame, and its show loop raises the cursor display counter by one per visible frame. Under Wine each call is a wineserver round trip; a profiled 113 ms gameplay frame spent 78% of its time in `ShowCursor`.
5. **The dead ad SDK** still makes network and connectivity calls on the main thread (profiled at 120 ms frames).
6. **Frame latency 1.** The game hardcodes `SetMaximumFrameLatency(1)`, which leaves no slack when the GPU or shader compiler hiccups.

## What dxhrfix does

| fix | key | patch |
|---|---|---|
| Create a queued resource inline on the waiting thread instead of sleeping (the D3D11 device is free threaded); a destroyed queued resource is removed safely | `hooks` | hooks on `awaitResource` 0x54C800 and `cancelOrAwait` 0x54C850 |
| Publish queue slots in order under the engine's own lock | `hooks` | request 0x54C7D0 replaced |
| Create each loaded shader library's pixel shaders ahead of time on a small thread pool | `precreate` | hooks on 0x54F000, 0x54F080, ~PixelShader 0x54C910, shutdown 0x54A440; helpers 0x5535F0, 0x54C8D0 replaced |
| Frame latency 2 | `frame_latency` | immediate at 0x54AB84 |
| Pass only real cursor changes to Windows | `cursor_fix` | import table slots of ShowCursor, SetCursor, ClipCursor |
| Built-in stub instead of the ad SDK | `ad_sdk=0` | none (the game handles a missing SDK) |
| Optional sampling profiler that explains slow frames | `profiler=1` | hook on the Present wrapper 0x54C9F0, ReadFile import slot |

## Install

Download `dxhrfix-0.4.0.zip` from the releases page. Close the game, then extract the `dxhrfix-0.4.0` folder into the game folder (or anywhere else). Do not extract the files loose into the game folder: that overwrites the original `DFEngine.dll` before it can be backed up (the installer detects this and tells you how to recover).

**Linux (Steam with Proton):**

```
cd "$HOME/.local/share/Steam/steamapps/common/Deus Ex Human Revolution Director's Cut/dxhrfix-0.4.0"
./install.sh
```

or `./install.sh "/path/to/Deus Ex Human Revolution Director's Cut"` from anywhere. No launch options are needed.

**Windows:** double click `install.bat` in the extracted folder, or run `install.bat "C:\path\to\Deus Ex Human Revolution Director's Cut"`.

The installer checks `DXHRDC.exe` (asks before continuing on an unknown build), checks that `DFEngine.dll` is the stock ad SDK DLL (sha256 `030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e`) or an earlier dxhrfix, keeps the stock DLL as `DFEngine_orig.dll`, copies the new DLL, copies `dxhrfix.ini` only if none exists, and copies `dxhrfix_syms.tsv`. It never deletes anything. Updating is the same procedure with the newer release.

After the first start, `dxhrfix.log` appears in the game folder. `hooks: 10 patches applied` and no `FAILED`, `SKIPPED` or `mismatch` lines means everything is active.

## Configuration

`dxhrfix.ini` in the game folder, section `[dxhrfix]`. Every key is documented in the file. Release defaults:

| key | default | meaning |
|---|---|---|
| hooks | 1 | master switch; 0 = patch nothing |
| precreate | 1 | shader precreate pool |
| pool_threads | auto | pool size; auto = min(4, cores - 2), at least 1; max 8 |
| precreate_max_per_sec | 0 | cap on pool creations per second; 0 = unlimited |
| frame_latency | 2 | 1..4; 1 = stock |
| cursor_fix | 1 | cursor call dedupe |
| cursor_refresh_ms | 250 | resend an unchanged cursor or clip rect at least this often |
| ad_sdk | 0 | 1 = forward to the original ad SDK (`DFEngine_orig.dll`) |
| log_level | 1 | 0, 1 or 2 |
| profiler | 0 | hitch profiler, writes `dxhrfix_hitch.log` |
| profiler_hz | 250 | samples per second per thread |
| profiler_hitch_ms | 40 | frames longer than this are reported |
| profiler_max_dumps | 200 | reports per session |
| profiler_ring_ms | 1000 | sample history in ms |
| profiler_extra_threads | 2 | 0 core threads, 1 plus game threads, 2 every thread including DXVK's |
| profiler_readfile | 1 | count ReadFile calls per thread |

A missing key uses the built-in default, which is the same except `ad_sdk` (built-in 1, forwarding like stock).

## Revert

Run `revert.sh` or `revert.bat` from the release folder: it copies `DFEngine_orig.dll` back over `DFEngine.dll` and changes nothing else. Steam's "Verify integrity of game files" also restores the stock DLL. The leftover dxhrfix files are ignored by the stock DLL.

## Reporting a bug

Open an issue with:

1. what happened and where in the game;
2. `dxhrfix.log` from the game folder;
3. for stutter: set `profiler=1` in `dxhrfix.ini`, reproduce, quit, and attach `dxhrfix_hitch.log` together with the `dxhrfix.log` of that session (then set `profiler=0` again);
4. Windows or Linux (Proton version), GPU and driver.

How to read both logs is described in [`dist/README.md`](dist/README.md).

## Building from source

Requirements: `i686-w64-mingw32-g++` 13 (MinGW-w64, win32 thread model is fine), GNU make, and Wine for the tests. On Debian or Ubuntu: `g++-mingw-w64-i686`, `make`, `wine`.

```
make          # builds dist/DFEngine.dll
make test     # hooklib and profiler unit tests, run under wine (wine prefixes in build/)
```

`make harness` runs `dev/harness/test_launch.sh`, which launches the real game under Proton with the freshly built DLL for 60 s and restores the game folder afterwards. Read the script header first and set `STEAMAPPS`, `DXHR_GAME`, `PROTON_DIR` and `COMPAT` for your machine.

`proxy/` also builds a stage 1 variant without patches (`make -C proxy`, `make -C proxy test`), useful to check the forwarding and stub logic in isolation.

## Repository layout

| path | content |
|---|---|
| `proxy/` | the `DFEngine.dll` proxy: `GetDFEngine` export, forwarding to `DFEngine_orig.dll` or stub, ini, log, startup |
| `hooklib/` | dependency free x86-32 inline hook library with its tests |
| `src/` | the fixes: `dxhrfix_hooks.cpp` (Hook A, frame latency), `dxhrfix_precreate.cpp` (Hook B), `dxhrfix_cursor.cpp`, `dxhrfix_profiler.cpp` and `dxhrfix_prof_core.*` (profiler), `test/` |
| `spec/` | developer documentation: engine structures, hook sites with expected bytes, threading, streaming notes |
| `dist/` | release files: built DLL, ini, symbol file, install and revert scripts, user README |
| `tools/gen_syms.py` | builds `dist/dxhrfix_syms.tsv` from a Ghidra function list and the exe's RTTI |
| `tools/ghidra/` | scripts and instructions to regenerate the analysis from your own exe; [`ADDRESSES.md`](tools/ghidra/ADDRESSES.md) lists the functions to read first |
| `dev/` | Linux test harness and the A/B measurement protocol (`TEST_PROTOCOL.md`) |
| `CHANGELOG.md` | version history |

## Related work

- [rrika/cdcEngineDXHR](https://github.com/rrika/cdcEngineDXHR), a source reconstruction of the engine, helped confirm class layouts.
- [HRDCfix](https://github.com/imring/HRDCfix) is an earlier fix for the Windows `Sleep(1)` polling.

## Credits

<!-- to be filled in by the maintainer -->

## License

MIT, see [`LICENSE`](LICENSE). Deus Ex: Human Revolution is a trademark of its owners; this project is not affiliated with or endorsed by them.
