#!/usr/bin/env bash
# dxhrfix: put the original DFEngine.dll back.
#
# Usage: ./revert.sh [game folder]
#   With no argument the game folder is the folder this script is in, or its
#   parent folder (same rule as install.sh).
#
# Copies DFEngine_orig.dll over DFEngine.dll and changes nothing else.
# DFEngine_orig.dll, dxhrfix.ini, dxhrfix_syms.tsv and the logs stay in the
# game folder; the stock DLL ignores them. Steam > Properties > Installed
# Files > Verify integrity also restores the stock DLL.
set -u

DFE_STOCK_SHA=030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e

die() { echo "ERROR: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

HERE=$(cd "$(dirname "$0")" && pwd) || die "cannot find the script folder"

if [ $# -ge 1 ]; then
    GAME=$1
elif [ -f "$HERE/DXHRDC.exe" ]; then
    GAME=$HERE
elif [ -f "$HERE/../DXHRDC.exe" ]; then
    GAME=$(cd "$HERE/.." && pwd)
else
    die "DXHRDC.exe not found next to this script or in its parent folder. Usage: $0 \"/path/to/Deus Ex Human Revolution Director's Cut\""
fi

echo "dxhrfix revert"
echo "game folder: $GAME"
[ -f "$GAME/DXHRDC.exe" ] || die "DXHRDC.exe not found in the game folder"
[ -f "$GAME/DFEngine_orig.dll" ] || die "DFEngine_orig.dll not found, nothing to revert to (use Steam Verify integrity instead)"
if command -v pgrep >/dev/null && pgrep -x DXHRDC.exe >/dev/null; then
    die "the game is running, close it first"
fi
o=$(sha "$GAME/DFEngine_orig.dll")
[ "$o" = "$DFE_STOCK_SHA" ] || echo "WARNING: DFEngine_orig.dll is not the stock DLL (sha256 $o)"
echo "copying DFEngine_orig.dll over DFEngine.dll"
cp -p "$GAME/DFEngine_orig.dll" "$GAME/DFEngine.dll" || die "copy failed"
[ "$(sha "$GAME/DFEngine.dll")" = "$o" ] || die "copy does not verify"
echo "done. The game now runs with the original DFEngine.dll."
