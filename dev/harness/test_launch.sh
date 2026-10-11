#!/usr/bin/env bash
# Usage: test_launch.sh <dll path> [seconds]
# Temporarily installs <dll> as the game's DFEngine.dll, launches DXHRDC.exe
# through Proton, checks it stays alive for N seconds (default 60), kills it,
# restores the game folder (EXIT trap) and prints a verdict.
#
# Two folder states are supported:
#   stock:     DFEngine.dll is the stock ad DLL and there is no
#              DFEngine_orig.dll. It is renamed to DFEngine_orig.dll for the
#              run and put back afterwards.
#   installed: DFEngine_orig.dll exists and is the stock ad DLL (the user ran
#              install.sh). The current DFEngine.dll is saved aside, the DLL
#              under test is copied in, and the saved one is copied back.
# In both states dxhrfix.ini, dxhrfix.log and dxhrfix_hitch.log of the user
# are saved aside before the run and put back afterwards; the run's logs go
# to harness/logs. A manifest (name, size, mtime, sha256 of every file except
# BIGFILE.*) is taken before and after, and the run FAILS unless both match.
# Nothing in the game folder is deleted except files this script created.
#
# Env:
#   STUB_TEST=1  hide DFEngine_orig.dll during the run (stock state only) so
#                the proxy's stub engine path is exercised.
#   TEST_INI=f   use file f as dxhrfix.ini for the run (the user's ini is
#                restored afterwards and verified by sha256).
#   SYMS=f       symbol file passed to the DLL via DXHRFIX_SYMS (default:
#                dxhrfix_syms.tsv next to the DLL under test, if present).
#   STEAMAPPS=d  Steam library steamapps folder (default
#                ~/.local/share/Steam/steamapps).
#   DXHR_GAME=d  game folder (default $STEAMAPPS/common/Deus Ex Human
#                Revolution Director's Cut).
#   PROTON_DIR=d Proton install (default $STEAMAPPS/common/Proton 11.0).
#   COMPAT=d     Proton prefix of app 238010 (default
#                $STEAMAPPS/compatdata/238010).
#   DXHRFIX_BACKUPS=d  where the stock DFEngine.dll copy and per run saves
#                are kept (default dev/harness/backups, git ignored).
set -u

DLL_SRC=${1:?usage: test_launch.sh <dll path> [seconds]}
SECS=${2:-60}
STARTUP_MAX=${STARTUP_MAX:-60}   # seconds to wait for DXHRDC.exe to appear

HERE=$(cd "$(dirname "$0")" && pwd)
# Paths; override through the environment for your machine.
STEAMAPPS=${STEAMAPPS:-$HOME/.local/share/Steam/steamapps}
GAME=${DXHR_GAME:-"$STEAMAPPS/common/Deus Ex Human Revolution Director's Cut"}
PROTON_DIR=${PROTON_DIR:-"$STEAMAPPS/common/Proton 11.0"}
PROTON="$PROTON_DIR/proton"
WINESERVER="$PROTON_DIR/files/bin/wineserver"
COMPAT=${COMPAT:-$STEAMAPPS/compatdata/238010}
BACKUPS=${DXHRFIX_BACKUPS:-$HERE/backups}
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
sha() { sha256sum "$1" | cut -d' ' -f1; }
SAVE=$BACKUPS/harness-$TS
mkdir -p "$SAVE" || die "cannot create $SAVE"

manifest() {
    (cd "$GAME" && for f in * .[!.]*; do
        [ -e "$f" ] || continue
        st=$(stat -c '%s %Y' -- "$f")
        case "$f" in BIGFILE.*) h=- ;; *) [ -f "$f" ] && h=$(sha "$f") || h=dir ;; esac
        echo "$f $st $h"
    done)
}

# 0. folder state
if [ -e "$GAME/DFEngine_orig.dll" ]; then
    [ "$(sha "$GAME/DFEngine_orig.dll")" = "$STOCK_SHA" ] || die "DFEngine_orig.dll exists but is not the stock ad DLL"
    MODE=installed
else
    MODE=stock
fi
echo "game folder state: $MODE"
manifest > "$SAVE/manifest.before" || die "manifest failed"

# 1. stock backup (only from a verified stock DLL)
if [ ! -f "$BACKUPS/DFEngine.dll.stock" ]; then
    [ "$MODE" = stock ] || die "no $BACKUPS/DFEngine.dll.stock; run once from a stock folder or copy DFEngine_orig.dll there"
    cur=$(sha "$GAME/DFEngine.dll")
    [ "$cur" = "$STOCK_SHA" ] || die "game DFEngine.dll is not stock ($cur) and no backup exists"
    cp -p "$GAME/DFEngine.dll" "$BACKUPS/DFEngine.dll.stock"
    (cd "$BACKUPS" && sha256sum DFEngine.dll.stock > DFEngine.dll.stock.sha256)
fi
(cd "$BACKUPS" && sha256sum -c --quiet DFEngine.dll.stock.sha256) || die "stock backup checksum mismatch"
if [ "$MODE" = stock ]; then
    [ "$(sha "$GAME/DFEngine.dll")" = "$STOCK_SHA" ] || die "stock state but DFEngine.dll is not stock"
fi

# symbols for the profiler (Wine path)
SYMS=${SYMS:-$(dirname "$DLL_SRC")/dxhrfix_syms.tsv}
if [ -f "$SYMS" ]; then
    SYMS_WIN="Z:$(readlink -f "$SYMS" | sed 's#/#\\#g')"
else
    SYMS_WIN=
fi

# 2. wait for any Steam shader replay of 238010
for _ in $(seq 1 60); do
    last=$(grep "238010\|Still replaying\|Done;" "$SHADERLOG" 2>/dev/null | tail -1)
    case "$last" in *"Still replaying"*) echo "waiting for shader replay..."; sleep 10 ;; *) break ;; esac
done

CREATED_ORIG=0
SWAPPED=0
SAVED_DLL=0
SAVED_INI=0
TEST_INI_PLACED=0
SAVED_LOGS=
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
    # DLL
    if [ "$SAVED_DLL" = 1 ]; then
        cp -p "$SAVE/DFEngine.dll" "$GAME/DFEngine.dll.restore.tmp" &&
            mv -f "$GAME/DFEngine.dll.restore.tmp" "$GAME/DFEngine.dll"
    elif [ "$SWAPPED" = 1 ] || [ "$CREATED_ORIG" = 1 ]; then
        cp -p "$BACKUPS/DFEngine.dll.stock" "$GAME/DFEngine.dll.restore.tmp" &&
            mv -f "$GAME/DFEngine.dll.restore.tmp" "$GAME/DFEngine.dll"
        [ "$CREATED_ORIG" = 1 ] && rm -f "$GAME/DFEngine_orig.dll"
    fi
    # run logs out, user logs back
    for f in dxhrfix.log dxhrfix_hitch.log; do
        [ -f "$GAME/$f" ] && mv -f "$GAME/$f" "$LOGDIR/$TS.$f"
    done
    for f in $SAVED_LOGS; do
        mv -f "$SAVE/$f" "$GAME/$f"
    done
    # ini
    if [ "$SAVED_INI" = 1 ]; then
        cp -p "$SAVE/dxhrfix.ini" "$GAME/dxhrfix.ini.restore.tmp" &&
            mv -f "$GAME/dxhrfix.ini.restore.tmp" "$GAME/dxhrfix.ini"
    elif [ "$TEST_INI_PLACED" = 1 ]; then
        rm -f "$GAME/dxhrfix.ini"   # created by this script, no user ini existed
    fi
    now=$(sha "$GAME/DFEngine.dll")
    if [ "$MODE" = installed ]; then
        if [ "$now" = "$(sha "$SAVE/DFEngine.dll")" ]; then echo "restore: DFEngine.dll is the saved installed DLL ($now)"; else echo "restore: WARNING DFEngine.dll sha $now"; fi
    else
        if [ "$now" = "$STOCK_SHA" ]; then echo "restore: DFEngine.dll is stock"; else echo "restore: WARNING DFEngine.dll sha $now"; fi
        [ -e "$GAME/DFEngine_orig.dll" ] && [ "$CREATED_ORIG" = 1 ] && echo "restore: WARNING DFEngine_orig.dll still present"
    fi
    if [ "$SAVED_INI" = 1 ]; then
        if [ "$(sha "$GAME/dxhrfix.ini")" = "$(sha "$SAVE/dxhrfix.ini")" ]; then echo "restore: dxhrfix.ini is the user's ini"; else echo "restore: WARNING dxhrfix.ini differs"; fi
    fi
    manifest > "$SAVE/manifest.after"
    if cmp -s "$SAVE/manifest.before" "$SAVE/manifest.after"; then
        echo "restore: MANIFEST MATCH (game folder identical to before the run)"
    else
        echo "restore: MANIFEST MISMATCH"
        diff "$SAVE/manifest.before" "$SAVE/manifest.after"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# 3. save user files, swap
for f in dxhrfix.log dxhrfix_hitch.log; do
    if [ -e "$GAME/$f" ]; then
        mv "$GAME/$f" "$SAVE/$f" || die "cannot move $f aside"
        SAVED_LOGS="$SAVED_LOGS $f"
    fi
done
if [ -n "${TEST_INI:-}" ]; then
    [ -f "$TEST_INI" ] || die "TEST_INI not found: $TEST_INI"
    if [ -e "$GAME/dxhrfix.ini" ]; then
        cp -p "$GAME/dxhrfix.ini" "$SAVE/dxhrfix.ini" || die "cannot save dxhrfix.ini"
        SAVED_INI=1
    else
        TEST_INI_PLACED=1
    fi
    cp "$TEST_INI" "$GAME/dxhrfix.ini.test.tmp" && mv -f "$GAME/dxhrfix.ini.test.tmp" "$GAME/dxhrfix.ini" || die "cannot place TEST_INI"
    TEST_INI_PLACED=1
    echo "TEST_INI: $TEST_INI in place"
fi
if [ "$MODE" = installed ]; then
    cp -p "$GAME/DFEngine.dll" "$SAVE/DFEngine.dll" || die "cannot save DFEngine.dll"
    [ "$(sha "$SAVE/DFEngine.dll")" = "$(sha "$GAME/DFEngine.dll")" ] || die "saved DFEngine.dll does not verify"
    SAVED_DLL=1
    echo "saved installed DFEngine.dll ($(sha "$SAVE/DFEngine.dll")) to $SAVE"
elif [ ! -e "$GAME/DFEngine_orig.dll" ]; then
    mv "$GAME/DFEngine.dll" "$GAME/DFEngine_orig.dll" || die "rename failed"
    CREATED_ORIG=1
fi
cp "$DLL_SRC" "$GAME/DFEngine.dll.test.tmp" && mv -f "$GAME/DFEngine.dll.test.tmp" "$GAME/DFEngine.dll" || die "copy failed"
SWAPPED=1
if [ "${STUB_TEST:-0}" = 1 ]; then
    [ "$CREATED_ORIG" = 1 ] || die "STUB_TEST needs DFEngine_orig.dll to be created by this script"
    rm -f "$GAME/DFEngine_orig.dll"   # stock copy stays in $BACKUPS; cleanup restores DFEngine.dll
    echo "STUB_TEST: DFEngine_orig.dll hidden"
fi
echo "installed $(sha "$DLL_SRC") as DFEngine.dll"
[ -n "$SYMS_WIN" ] && echo "symbols: DXHRFIX_SYMS=$SYMS_WIN"

# 4. launch
rm -f "$STEAMLOG"
(
    cd "$GAME" &&
    STEAM_COMPAT_DATA_PATH=$COMPAT \
    STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam \
    SteamAppId=238010 SteamGameId=238010 DISPLAY=:0 PROTON_LOG=1 \
    DXHRFIX_SYMS=$SYMS_WIN \
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
unh=$(grep -cE "wine: Unhandled exception|:seh:.*Unhandled exception" "$L")
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
if [ -f "$LOGDIR/$TS.dxhrfix_hitch.log" ]; then
    echo "----- dxhrfix_hitch.log: $LOGDIR/$TS.dxhrfix_hitch.log ($(grep -c '^==================== hitch' "$LOGDIR/$TS.dxhrfix_hitch.log") dumps) -----"
fi
manifest_ok=0
echo "$cleanup_out" | grep -q "MANIFEST MATCH" && manifest_ok=1
echo "manifest_match: $manifest_ok"
pass=0
[ "$seen" = 1 ] && [ "$alive" -ge 45 ] && [ "$early_exit" = 0 ] && [ "$unh" = 0 ] && [ "$manifest_ok" = 1 ] && pass=1
echo "RESULT: $([ $pass = 1 ] && echo PASS || echo FAIL)"
[ $pass = 1 ]
