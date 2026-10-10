#!/usr/bin/env bash
# Put the original DFEngine.dll back (copies DFEngine_orig.dll over
# DFEngine.dll). Never deletes anything: DFEngine_orig.dll, dxhrfix.ini and
# dxhrfix.log stay where they are (they are harmless with the stock DLL).
# Usage: ./revert.sh [game folder]
set -u

GAME=${1:-"/mnt/Backup/SteamLibrary/steamapps/common/Deus Ex Human Revolution Director's Cut"}
DFE_STOCK=030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e

die() { echo "ERROR: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

echo "game folder: $GAME"
[ -f "$GAME/DFEngine_orig.dll" ] || die "DFEngine_orig.dll not found, nothing to revert to"
if pgrep -x DXHRDC.exe >/dev/null; then die "the game is running, close it first"; fi
o=$(sha "$GAME/DFEngine_orig.dll")
echo "DFEngine_orig.dll sha256 $o"
[ "$o" = "$DFE_STOCK" ] || echo "WARNING: DFEngine_orig.dll is not the stock DFEngine.dll"
echo "copying DFEngine_orig.dll over DFEngine.dll"
cp -p "$GAME/DFEngine_orig.dll" "$GAME/DFEngine.dll" || die "copy failed"
n=$(sha "$GAME/DFEngine.dll")
echo "DFEngine.dll sha256 now $n"
[ "$n" = "$o" ] || die "copy does not verify"
echo "done. DFEngine_orig.dll, dxhrfix.ini and dxhrfix.log were left in place."
