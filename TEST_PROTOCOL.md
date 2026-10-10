# dxhrfix test protocol (A/B with MangoHud)

## 1. Install

```
cd /mnt/Backup/dxhrdc-fix/dll/dist
./install.sh
```

The game must be closed. The script refuses a non-stock exe and prints each step.

## 2. Launch options

Steam > Deus Ex: Human Revolution Director's Cut > Properties > General > Launch Options:

```
MANGOHUD=1 MANGOHUD_CONFIGFILE=/mnt/Backup/dxhrdc-fix/apply/MangoHud.conf %command%
```

That config caps at 72 fps, logs every frame (`log_interval=0`), starts logging automatically (`autostart_log=2`), stops after 180 s (`log_duration=180`) and writes the CSV to `/mnt/Backup/dxhrdc-fix/logs`.

Before testing:
- Turn off Steam > Settings > Downloads > "Allow background processing of Vulkan shaders".
- Close heavy apps.

## 3. Run

1. Start the game and load the same Detroit save used for runs A and E.
2. Walk the same Detroit route for 3 minutes. The MangoHud log stops by itself after 180 s.
3. Quit the game.
4. The new CSV is the newest `DXHRDC_*.csv` in `/mnt/Backup/dxhrdc-fix/logs`:

```
ls -t /mnt/Backup/dxhrdc-fix/logs/DXHRDC_*.csv | head -1
```

## 4. Compare

```
python3 -I /mnt/Backup/dxhrdc-fix/apply/analyze_mangohud.py \
  A=/mnt/Backup/dxhrdc-fix/logs/DXHRDC_2026-10-10_11-09-10.csv \
  E=/mnt/Backup/dxhrdc-fix/logs/DXHRDC_2026-10-10_13-18-40.csv \
  F=<new csv>
```

A and E are the earlier reference runs and F is this build. Look at the hitch counts and the high-percentile frametimes (p99 and the worst frames). Average fps matters less.

## 5. Read dxhrfix.log

The log is in the game folder: `/mnt/Backup/SteamLibrary/steamapps/common/Deus Ex Human Revolution Director's Cut/dxhrfix.log`.

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
G="/mnt/Backup/SteamLibrary/steamapps/common/Deus Ex Human Revolution Director's Cut"
grep -E "FAILED|SKIPPED|mismatch|still waiting" "$G/dxhrfix.log"
grep -E "^.{12} \[[0-9]+\] (totals|precreate):" "$G/dxhrfix.log" | tail -2
```

## 6. Variants (optional)

Edit `dxhrfix.ini` in the game folder, then repeat steps 3 to 5:
- `precreate=0`: Hook A only.
- `frame_latency=1`: stock latency.
- `hooks=0`: everything stock except the DLL swap.

## 7. Revert

```
cd /mnt/Backup/dxhrdc-fix/dll/dist && ./revert.sh
```

Alternatively, use Steam Verify Integrity. Restore the Launch Options to your original value: `mangohud %command%`.
