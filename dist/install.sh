#!/usr/bin/env bash
# Install the dxhrfix DFEngine.dll proxy into the game folder.
# Usage: ./install.sh [game folder]
# Never deletes anything. The original DFEngine.dll is kept as DFEngine_orig.dll.
set -u

GAME=${1:-"/mnt/Backup/SteamLibrary/steamapps/common/Deus Ex Human Revolution Director's Cut"}
HERE=$(cd "$(dirname "$0")" && pwd)
EXE_STOCK=8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1
DFE_STOCK=030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e
STOCK_FILE=/mnt/Backup/dxhrdc-fix/backups/DFEngine.dll.stock.sha256

die() { echo "ERROR: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

echo "game folder: $GAME"
[ -f "$GAME/DXHRDC.exe" ] || die "DXHRDC.exe not found in the game folder"
[ -f "$HERE/DFEngine.dll" ] || die "DFEngine.dll (proxy) not found next to this script"
[ -f "$HERE/dxhrfix.ini" ] || die "dxhrfix.ini not found next to this script"
if pgrep -x DXHRDC.exe >/dev/null; then die "the game is running, close it first"; fi

if [ -f "$STOCK_FILE" ]; then
    f=$(cut -d' ' -f1 "$STOCK_FILE")
    [ "$f" = "$DFE_STOCK" ] || die "$STOCK_FILE ($f) disagrees with the built-in stock hash"
    echo "step 1: stock DFEngine.dll hash confirmed by $STOCK_FILE"
else
    echo "step 1: $STOCK_FILE not found, using the built-in stock hash"
fi

echo "step 2: checking DXHRDC.exe"
e=$(sha "$GAME/DXHRDC.exe")
[ "$e" = "$EXE_STOCK" ] || die "DXHRDC.exe sha256 $e is not the stock exe; refusing (the hooks are built for the stock exe)"
echo "        DXHRDC.exe is stock"

if [ -e "$GAME/DFEngine_orig.dll" ]; then
    o=$(sha "$GAME/DFEngine_orig.dll")
    echo "step 3: DFEngine_orig.dll already exists (sha256 $o), keeping it"
    [ "$o" = "$DFE_STOCK" ] || echo "        WARNING: DFEngine_orig.dll is not the stock DFEngine.dll"
else
    d=$(sha "$GAME/DFEngine.dll")
    [ "$d" = "$DFE_STOCK" ] || die "game DFEngine.dll sha256 $d is not stock and no DFEngine_orig.dll exists; refusing"
    echo "step 3: copying stock DFEngine.dll to DFEngine_orig.dll"
    cp -p "$GAME/DFEngine.dll" "$GAME/DFEngine_orig.dll" || die "copy failed"
    [ "$(sha "$GAME/DFEngine_orig.dll")" = "$DFE_STOCK" ] || die "DFEngine_orig.dll copy does not verify"
fi

echo "step 4: copying proxy DFEngine.dll into the game folder"
cp "$HERE/DFEngine.dll" "$GAME/DFEngine.dll" || die "copy failed"
[ "$(sha "$GAME/DFEngine.dll")" = "$(sha "$HERE/DFEngine.dll")" ] || die "proxy copy does not verify"

if [ -e "$GAME/dxhrfix.ini" ]; then
    echo "step 5: dxhrfix.ini already exists in the game folder, keeping it (packaged defaults: $HERE/dxhrfix.ini)"
else
    echo "step 5: copying dxhrfix.ini into the game folder"
    cp "$HERE/dxhrfix.ini" "$GAME/dxhrfix.ini" || die "copy failed"
fi

echo "done. Start the game, then check $GAME/dxhrfix.log"
