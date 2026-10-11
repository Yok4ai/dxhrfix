#!/usr/bin/env bash
# dxhrfix installer for Deus Ex: Human Revolution Director's Cut (Steam).
#
# Usage: ./install.sh [game folder]
#   With no argument the game folder is the folder this script is in, or its
#   parent folder (for example when the release was extracted into a
#   subfolder of the game folder). The game folder is the one that holds
#   DXHRDC.exe.
#
# What it does:
#   - checks DXHRDC.exe against the supported Steam build (asks before going
#     on if it differs; the patches check the game's bytes themselves and
#     switch off on a mismatch);
#   - keeps the stock DFEngine.dll as DFEngine_orig.dll;
#   - copies the dxhrfix DFEngine.dll in (also used to update an older
#     dxhrfix install);
#   - copies dxhrfix.ini only if there is none yet (your settings are kept);
#   - copies dxhrfix_syms.tsv (function names for the optional profiler).
# It never deletes anything. Undo with revert.sh.
set -u

EXE_SHA=8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1
DFE_STOCK_SHA=030324902ef29f04a3c1152fac6f52bd26ba64dbdaa71cf24f6d2f97e102145e
MARKER='dxhrfix proxy'

die() { echo "ERROR: $*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
is_proxy() { grep -qaF "$MARKER" "$1"; }

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

echo "dxhrfix installer"
echo "game folder: $GAME"
[ -d "$GAME" ] || die "not a folder: $GAME"
[ -f "$GAME/DXHRDC.exe" ] || die "DXHRDC.exe not found in the game folder"
[ -f "$GAME/DFEngine.dll" ] || die "DFEngine.dll not found in the game folder"
[ -f "$HERE/DFEngine.dll" ] || die "DFEngine.dll (dxhrfix) not found next to this script"
[ -f "$HERE/dxhrfix.ini" ] || die "dxhrfix.ini not found next to this script"
is_proxy "$HERE/DFEngine.dll" || die "$HERE/DFEngine.dll is not the dxhrfix DLL"
if [ "$GAME/DFEngine.dll" -ef "$HERE/DFEngine.dll" ]; then
    die "the release files are in the game folder itself, so the game's DFEngine.dll was replaced when they were extracted.
       Run Steam > Properties > Installed Files > Verify integrity to get the stock DFEngine.dll back,
       then extract the release into its own folder (for example a subfolder of the game folder) and run install.sh from there."
fi
if command -v pgrep >/dev/null && pgrep -x DXHRDC.exe >/dev/null; then
    die "the game is running, close it first"
fi

echo "step 1: checking DXHRDC.exe"
e=$(sha "$GAME/DXHRDC.exe")
if [ "$e" = "$EXE_SHA" ]; then
    echo "        DXHRDC.exe is the supported Steam build"
else
    echo "WARNING: DXHRDC.exe sha256 is $e"
    echo "         dxhrfix supports only the Steam build $EXE_SHA."
    echo "         Every patch checks the game's bytes first and switches itself off on a"
    echo "         mismatch, so the game should run stock, but this is untested."
    printf "Continue anyway? [y/N] "
    read -r answer || answer=
    case "$answer" in y|Y|yes|YES) echo "        continuing" ;; *) die "cancelled, nothing was changed" ;; esac
fi

echo "step 2: checking DFEngine.dll in the game folder"
d=$(sha "$GAME/DFEngine.dll")
if [ "$d" = "$DFE_STOCK_SHA" ]; then
    echo "        DFEngine.dll is the stock ad SDK DLL"
    if [ -e "$GAME/DFEngine_orig.dll" ]; then
        o=$(sha "$GAME/DFEngine_orig.dll")
        [ "$o" = "$DFE_STOCK_SHA" ] || die "DFEngine_orig.dll exists but is not the stock DLL (sha256 $o). Move it out of the game folder and run again."
        echo "        DFEngine_orig.dll already exists and is stock, keeping it"
    else
        echo "        keeping the stock DLL as DFEngine_orig.dll"
        cp -p "$GAME/DFEngine.dll" "$GAME/DFEngine_orig.dll" || die "copy failed"
        [ "$(sha "$GAME/DFEngine_orig.dll")" = "$DFE_STOCK_SHA" ] || die "DFEngine_orig.dll copy does not verify"
    fi
elif is_proxy "$GAME/DFEngine.dll"; then
    echo "        DFEngine.dll is an earlier dxhrfix, updating it"
    if [ -e "$GAME/DFEngine_orig.dll" ]; then
        o=$(sha "$GAME/DFEngine_orig.dll")
        [ "$o" = "$DFE_STOCK_SHA" ] || echo "WARNING: DFEngine_orig.dll is not the stock DLL (sha256 $o)"
    else
        echo "WARNING: DFEngine_orig.dll is missing, so revert.sh will not work."
        echo "         Steam > Properties > Installed Files > Verify integrity restores the stock DLL."
    fi
else
    die "DFEngine.dll in the game folder is neither the stock DLL nor dxhrfix (sha256 $d).
       Another mod may use it. Run Steam > Properties > Installed Files > Verify integrity, then run install.sh again."
fi

echo "step 3: copying dxhrfix DFEngine.dll into the game folder"
cp "$HERE/DFEngine.dll" "$GAME/DFEngine.dll" || die "copy failed"
[ "$(sha "$GAME/DFEngine.dll")" = "$(sha "$HERE/DFEngine.dll")" ] || die "DFEngine.dll copy does not verify"

if [ -e "$GAME/dxhrfix.ini" ]; then
    echo "step 4: dxhrfix.ini already exists in the game folder, keeping your settings"
    echo "        (release defaults for comparison: $HERE/dxhrfix.ini)"
else
    echo "step 4: copying dxhrfix.ini into the game folder"
    cp "$HERE/dxhrfix.ini" "$GAME/dxhrfix.ini" || die "copy failed"
fi

if [ -f "$HERE/dxhrfix_syms.tsv" ]; then
    echo "step 5: copying dxhrfix_syms.tsv (function names for the profiler) into the game folder"
    cp "$HERE/dxhrfix_syms.tsv" "$GAME/dxhrfix_syms.tsv" || die "copy failed"
else
    echo "step 5: dxhrfix_syms.tsv not found next to this script, skipped (only the profiler uses it)"
fi

echo "done. Start the game, then check that $GAME/dxhrfix.log exists."
