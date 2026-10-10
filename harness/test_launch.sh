#!/usr/bin/env bash
# Usage: test_launch.sh <dll path> [seconds]
# Temporarily installs <dll> as the game's DFEngine.dll, launches DXHRDC.exe
# through Proton, checks it stays alive for N seconds (default 60), kills it,
# restores the game folder (EXIT trap) and prints a verdict.
# Env: STUB_TEST=1 hides DFEngine_orig.dll during the run (only when this
# script created it) so the proxy's stub engine path is exercised.
set -u

DLL_SRC=${1:?usage: test_launch.sh <dll path> [seconds]}
SECS=${2:-60}
STARTUP_MAX=${STARTUP_MAX:-60}   # seconds to wait for DXHRDC.exe to appear

GAME="/mnt/Backup/SteamLibrary/steamapps/common/Deus Ex Human Revolution Director's Cut"
PROTON="/mnt/Backup/SteamLibrary/steamapps/common/Proton 11.0/proton"
WINESERVER="/mnt/Backup/SteamLibrary/steamapps/common/Proton 11.0/files/bin/wineserver"
COMPAT=/mnt/Backup/SteamLibrary/steamapps/compatdata/238010
BACKUPS=/mnt/Backup/dxhrdc-fix/backups
HERE=$(cd "$(dirname "$0")" && pwd)
LOGDIR=$HERE/logs
STEAMLOG=$HOME/steam-238010.log
SHADERLOG=$HOME/.local/share/Steam/logs/shader_log.txt
STOCK_SHA=030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e
TS=$(date +%Y%m%d-%H%M%S)

die() { echo "ERROR: $*" >&2; exit 2; }

[ -f "$DLL_SRC" ] || die "dll not found: $DLL_SRC"
DLL_SRC=$(readlink -f "$DLL_SRC")
[ -x "$PROTON" ] || die "proton not found"
[ -f "$GAME/DXHRDC.exe" ] || die "game not found"
pgrep -x DXHRDC.exe >/dev/null && die "DXHRDC.exe already running"
mkdir -p "$LOGDIR" "$BACKUPS"

# 1. stock backup (only from a verified stock DLL)
if [ ! -f "$BACKUPS/DFEngine.dll.stock" ]; then
    cur=$(sha256sum "$GAME/DFEngine.dll" | cut -d' ' -f1)
    [ "$cur" = "$STOCK_SHA" ] || die "game DFEngine.dll is not stock ($cur) and no backup exists"
    cp -p "$GAME/DFEngine.dll" "$BACKUPS/DFEngine.dll.stock"
    (cd "$BACKUPS" && sha256sum DFEngine.dll.stock > DFEngine.dll.stock.sha256)
fi
(cd "$BACKUPS" && sha256sum -c --quiet DFEngine.dll.stock.sha256) || die "stock backup checksum mismatch"

# 2. wait for any Steam shader replay of 238010
for _ in $(seq 1 60); do
    last=$(grep "238010\|Still replaying\|Done;" "$SHADERLOG" 2>/dev/null | tail -1)
    case "$last" in *"Still replaying"*) echo "waiting for shader replay..."; sleep 10 ;; *) break ;; esac
done

CREATED_ORIG=0
SWAPPED=0
GAME_PID=
cleanup() {
    set +e
    if pgrep -x DXHRDC.exe >/dev/null; then
        pkill -TERM -x DXHRDC.exe
        for _ in $(seq 1 10); do pgrep -x DXHRDC.exe >/dev/null || break; sleep 1; done
        pkill -KILL -x DXHRDC.exe
    fi
    WINEPREFIX=$COMPAT/pfx "$WINESERVER" -k 2>/dev/null
    for _ in $(seq 1 15); do
        WINEPREFIX=$COMPAT/pfx "$WINESERVER" -w 2>/dev/null && break
        sleep 1
    done
    [ -n "$GAME_PID" ] && kill "$GAME_PID" 2>/dev/null
    sleep 2
    if [ "$SWAPPED" = 1 ] || [ "$CREATED_ORIG" = 1 ]; then
        cp -p "$BACKUPS/DFEngine.dll.stock" "$GAME/DFEngine.dll.restore.tmp" &&
            mv -f "$GAME/DFEngine.dll.restore.tmp" "$GAME/DFEngine.dll"
        [ "$CREATED_ORIG" = 1 ] && rm -f "$GAME/DFEngine_orig.dll"
    fi
    if [ -f "$GAME/dxhrfix.log" ]; then
        mv -f "$GAME/dxhrfix.log" "$LOGDIR/$TS.dxhrfix.log"
    fi
    now=$(sha256sum "$GAME/DFEngine.dll" | cut -d' ' -f1)
    if [ "$now" = "$STOCK_SHA" ]; then echo "restore: DFEngine.dll is stock"; else echo "restore: WARNING DFEngine.dll sha $now"; fi
    [ -e "$GAME/DFEngine_orig.dll" ] && [ "$CREATED_ORIG" = 1 ] && echo "restore: WARNING DFEngine_orig.dll still present"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# 3. swap
if [ ! -e "$GAME/DFEngine_orig.dll" ]; then
    mv "$GAME/DFEngine.dll" "$GAME/DFEngine_orig.dll" || die "rename failed"
    CREATED_ORIG=1
fi
cp "$DLL_SRC" "$GAME/DFEngine.dll" || die "copy failed"
SWAPPED=1
if [ "${STUB_TEST:-0}" = 1 ]; then
    [ "$CREATED_ORIG" = 1 ] || die "STUB_TEST needs DFEngine_orig.dll to be created by this script"
    rm -f "$GAME/DFEngine_orig.dll"   # stock copy stays in $BACKUPS; cleanup restores DFEngine.dll
    echo "STUB_TEST: DFEngine_orig.dll hidden"
fi
rm -f "$GAME/dxhrfix.log"
echo "installed $(sha256sum "$DLL_SRC" | cut -d' ' -f1) as DFEngine.dll"

# 4. launch
rm -f "$STEAMLOG"
(
    cd "$GAME" &&
    STEAM_COMPAT_DATA_PATH=$COMPAT \
    STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam \
    SteamAppId=238010 SteamGameId=238010 DISPLAY=:0 PROTON_LOG=1 \
    exec "$PROTON" run "$GAME/DXHRDC.exe"
) >"$LOGDIR/$TS.stdout.log" 2>&1 &
GAME_PID=$!

t0=$(date +%s)
seen=0
alive=0
for _ in $(seq 1 "$STARTUP_MAX"); do
    if pgrep -x DXHRDC.exe >/dev/null; then seen=1; break; fi
    sleep 1
done
startup=$(( $(date +%s) - t0 ))
early_exit=0
if [ "$seen" = 1 ]; then
    t1=$(date +%s)
    while :; do
        alive=$(( $(date +%s) - t1 ))
        [ "$alive" -ge "$SECS" ] && break
        if ! pgrep -x DXHRDC.exe >/dev/null; then early_exit=1; break; fi
        sleep 1
    done
fi

cleanup_out=$(cleanup 2>&1)
trap - EXIT

cp -f "$STEAMLOG" "$LOGDIR/$TS.log" 2>/dev/null || echo "(no $STEAMLOG)" > "$LOGDIR/$TS.log"
L=$LOGDIR/$TS.log
unh=$(grep -c "Unhandled exception\|wine: Unhandled" "$L")
errs=$(grep -c "^[0-9a-f]*:err:\|^err:\|:err:" "$L")

echo "===== VERDICT ====="
echo "dll: $DLL_SRC"
echo "startup_seconds: $startup (seen=$seen)"
echo "alive_seconds: $alive (target $SECS, early_exit=$early_exit)"
echo "unhandled_exceptions: $unh"
echo "err_lines: $errs"
if [ "$errs" -gt 0 ]; then
    echo "distinct err lines (top 15):"
    grep ":err:\|^err:" "$L" | sed -E 's/^[0-9.]+:[0-9a-f]+:[0-9a-f]+://' | sort | uniq -c | sort -rn | head -15
fi
echo "steam_log: $L"
echo "$cleanup_out"
echo "----- dxhrfix.log -----"
if [ -f "$LOGDIR/$TS.dxhrfix.log" ]; then cat "$LOGDIR/$TS.dxhrfix.log"; else echo "(none)"; fi
pass=0
[ "$seen" = 1 ] && [ "$alive" -ge 45 ] && [ "$early_exit" = 0 ] && [ "$unh" = 0 ] && pass=1
echo "RESULT: $([ $pass = 1 ] && echo PASS || echo FAIL)"
[ $pass = 1 ]
