# dxhrfix test protocol (A/B with MangoHud)

Developer notes for measuring a build on Linux/Proton. Placeholders used below:

- `$REPO`: this repository.
- `$GAME`: the game folder (the one with `DXHRDC.exe`), for example `~/.local/share/Steam/steamapps/common/Deus Ex Human Revolution Director's Cut`.
- `$WORK`: a work folder of your choice for the MangoHud config, the frame time CSVs and the analysis script. The MangoHud config and `analyze_mangohud.py` used during development are not part of this repository; any frame time CSV analysis that counts hitches and high percentiles works.

## 1. Install

```
cd $REPO/dist
./install.sh "$GAME"
```

The game must be closed. The script warns about a non-stock exe and prints each step.

## 2. Launch options

Steam > Deus Ex: Human Revolution Director's Cut > Properties > General > Launch Options:

```
MANGOHUD=1 MANGOHUD_CONFIGFILE=$WORK/MangoHud.conf %command%
```

That config caps at 72 fps, logs every frame (`log_interval=0`), starts logging automatically (`autostart_log=2`), stops after 180 s (`log_duration=180`) and writes the CSV to `$WORK/logs` (`output_folder=`). Write the real path in the Steam launch options; Steam does not expand `$WORK`.

Before testing:
- Turn off Steam > Settings > Downloads > "Allow background processing of Vulkan shaders".
- Close heavy apps.

## 3. Run

1. Start the game and load the same Detroit save used for the reference runs.
2. Walk the same Detroit route for 3 minutes. The MangoHud log stops by itself after 180 s.
3. Quit the game.
4. The new CSV is the newest `DXHRDC_*.csv` in `$WORK/logs`:

```
ls -t $WORK/logs/DXHRDC_*.csv | head -1
```

## 4. Compare

```
python3 -I $WORK/analyze_mangohud.py \
  A=$WORK/logs/<stock run csv> \
  E=<cold cache stock run csv> \
  F=<new csv>
```

A and E are earlier stock reference runs and F is this build. Look at the hitch counts and the high-percentile frametimes (p99 and the worst frames). Average fps matters less.

## 5. Read dxhrfix.log

The log is in the game folder: `$GAME/dxhrfix.log`.

1. Check the install lines. You should see `hooks: 10 patches applied` and no `FAILED`, `SKIPPED` or `mismatch`.
2. Take the **last** pair of `totals:` / `precreate:` lines, written every 10 s, and note:
   - `inline` (stalls converted to an immediate create);
   - `fallback`, `max_wait` (time still spent waiting on the worker; lower is better);
   - `max_inline_create`;
   - `dtor_wait` and `skipped_unload` (level unload paths; this is the first real test of them);
   - `created` (shaders precreated by the pool), `lost`, `queued` (should drain to 0), `create_total`.
3. Check the `precreate lib #N ...` lines for the level's libraries. `wall` is the time from library load until all its shaders exist.
4. Check for `still waiting on item` lines. Any of these means a thread waited more than 1 s, so please report them.

Optional quick lines:

```
grep -E "FAILED|SKIPPED|mismatch|still waiting" "$GAME/dxhrfix.log"
grep -E "^.{12} \[[0-9]+\] (totals|precreate):" "$GAME/dxhrfix.log" | tail -2
```

## 5b. Read dxhrfix_hitch.log (hitch profiler)

Note: alt-tabbing or a window losing focus makes the game sleep in its inactive loop; those frames show up as multi-second hitches with the main thread in `Sleep` and `vkDestroySwapchainKHR`. Ignore them.

The hitch profiler is off by default; set `profiler=1` in `$GAME/dxhrfix.ini` for this step. Install with `./install.sh` so `dxhrfix_syms.tsv` lands next to `DFEngine.dll`. After the run, the reports are in the game folder: `dxhrfix_hitch.log`.

1. In `dxhrfix.log`, check the profiler lines: `hook present ... installed`, `profiler: tracking thread ... as main`, `... as render`, `... as async_create`, `... as fs_worker`, `profiler: sampler running at 250 Hz`, and the 10 s `profiler: ticks ...` lines (`fail` counts should stay 0; `dumps ... dropped` should be 0).
2. List the slow frames:

```
grep "^==================== hitch" "$GAME/dxhrfix_hitch.log"
```

3. For each of the ~10 frames of 100 ms or more, read its block (see dist/README.md, "Reading dxhrfix_hitch.log"):
   - **render** thread, leaf functions: if most samples sit in `ntdll.dll!NtWaitFor*` / `RtlWaitOnAddress` / `SleepConditionVariableSRW`, the render thread was blocked. The return addresses tell on what: game call sites like `renderThreadProc+0x34` (waiting for the main thread to hand over a frame) or a game function calling into `d3d11.dll` (DXVK: a D3D11 call that blocked inside the driver, for example a resource Map or a pipeline compile).
   - **main** thread: if render waited in `renderThreadProc`, the main thread is the one that was slow. Its leaf functions and top stacks show where (game logic, streaming, `DFEngine_orig.dll` ad SDK network calls, ReadFile, allocation).
   - **async_create** / **fs_worker**: busy or idle during the slow frame.
   - **dxvk** threads ('dxvk-cs', 'dxvk-submit', 'dxvk-shader-...'): if render was blocked inside DXVK Present, look at which DXVK thread was busy and its `native leaf by file`: `libnvidia-glcore` means the driver was compiling or working, `[vdso]` means it was waiting.
   - **ReadFile** section: large byte counts or long times on the main thread point to synchronous loading.
   - The `totals:` / `precreate:` lines show whether shader creation was active around that frame (compare with the previous report).
4. Please send `dxhrfix_hitch.log` and `dxhrfix.log` with the MangoHud CSV.

The profiler costs a little frame time (each tracked thread is suspended about 5% of the time at 250 Hz). For a clean A/B of frame times, run once more with `profiler=0` in `dxhrfix.ini`.

## 6. Variants (optional)

Edit `dxhrfix.ini` in the game folder, then repeat steps 3 to 5:
- `precreate=0`: Hook A only.
- `frame_latency=1`: stock latency.
- `hooks=0`: everything stock except the DLL swap. With `profiler=1` this gives a profile of the stock game.
- `ad_sdk=1`: forward the ad SDK to `DFEngine_orig.dll` as stock does.
- `cursor_fix=0`: stock cursor calls.

## 7. Revert

```
cd $REPO/dist && ./revert.sh "$GAME"
```

Alternatively, use Steam Verify Integrity. Restore the Launch Options to your original value.
