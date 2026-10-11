# Regenerating the analysis

This folder lets you redo the reverse engineering behind dxhrfix from your own copy of the game. The repository does not contain the Ghidra project, decompiled C, disassembly listings or any other part of the game's code; you produce those locally from your own `DXHRDC.exe` and they should stay on your machine.

Everything here is our own code: Ghidra scripts (`scripts/`), small Python and shell helpers that read the exports (`helpers/`), and address lists with our own labels (`lists/`). `ADDRESSES.md` lists the functions dxhrfix relies on, so you know what to decompile first.

## Requirements

- The Steam `DXHRDC.exe`, sha256 `8266b6b4a5bf25f2f4e8de068aa3720f6289c962bb1c2bb70a7b1c111ba510a1`. Check with `sha256sum DXHRDC.exe`. Other builds have different addresses.
- Java 21 (JDK, 64-bit). Ghidra 12.1 refuses older versions. Check with `java -version`. Any OpenJDK 21 build works, for example Eclipse Temurin 21.
- Ghidra 12.1.4. Other 12.x versions should work, but function boundaries and auto names can differ slightly, which changes `dxhrfix_syms.tsv`.
- Python 3.8 or newer, and GNU objdump (binutils, or `i686-w64-mingw32-objdump` from MinGW) for the helpers.
- About 4 GB of RAM for the analysis and 30 to 60 minutes of CPU time.

## 1. Get Ghidra and verify it

```
curl -LO https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.4_build/ghidra_12.1.4_PUBLIC_20260921.zip
echo "ddac49f903da9d5bac833e5cc79395098b9c33cfd3279be5f31bd00387d2d4db  ghidra_12.1.4_PUBLIC_20260921.zip" | sha256sum -c
unzip -q ghidra_12.1.4_PUBLIC_20260921.zip
export GHIDRA=$PWD/ghidra_12.1.4_PUBLIC
```

The checksum is the one published on the release page (https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.4_build). Do not continue if `sha256sum -c` does not print `OK`. On Windows, use `certutil -hashfile ghidra_12.1.4_PUBLIC_20260921.zip SHA256` and compare by eye, and run `analyzeHeadless.bat` instead of `analyzeHeadless` below.

## 2. Set up paths

Run the commands from the repository root.

```
export REPO=$PWD
export DXHR_EXE="/path/to/Deus Ex Human Revolution Director's Cut/DXHRDC.exe"
export DXHR_OUT=$HOME/dxhr-analysis/out      # exports land here (keep it private)
export DXHR_PROJ=$HOME/dxhr-analysis/proj    # Ghidra project folder
mkdir -p "$DXHR_OUT" "$DXHR_PROJ"
sha256sum "$DXHR_EXE"
```

Keep `DXHR_OUT` and `DXHR_PROJ` outside the repository, so nothing derived from the game gets committed.

## 3. Import, analyze, export the function list and xrefs

One command imports the exe into a fresh project named `dxhr`, runs the full auto analysis and then `ExportBasics.java`:

```
"$GHIDRA/support/analyzeHeadless" "$DXHR_PROJ" dxhr \
  -import "$DXHR_EXE" \
  -scriptPath "$REPO/tools/ghidra/scripts" \
  -postScript ExportBasics.java "$DXHR_OUT" \
  -analysisTimeoutPerFile 7200 \
  -log "$DXHR_OUT/ghidra_import.log"
```

The program keeps the file name, so the outputs are named after it (do not rename the exe):

| file | content |
|---|---|
| `DXHRDC.exe.functions.tsv` | every function: entry VA, Ghidra name, size in bytes |
| `DXHRDC.exe.strings_xrefs.tsv` | every defined string with the code that references it |
| `DXHRDC.exe.imports_callers.tsv` | every imported function with its IAT slot and callers |

To rerun only the export on the analyzed project:

```
"$GHIDRA/support/analyzeHeadless" "$DXHR_PROJ" dxhr -process DXHRDC.exe -noanalysis -readOnly \
  -scriptPath "$REPO/tools/ghidra/scripts" -postScript ExportBasics.java "$DXHR_OUT"
```

## 4. Decompile a list of functions

`Decomp.java <list> <out.c>` decompiles every function named in a list file. The list has one hex VA per line, optionally followed by a tab and a label; lines starting with `#` are skipped. Any address inside a function selects that function.

```
"$GHIDRA/support/analyzeHeadless" "$DXHR_PROJ" dxhr -process DXHRDC.exe -noanalysis -readOnly \
  -scriptPath "$REPO/tools/ghidra/scripts" \
  -postScript Decomp.java "$REPO/tools/ghidra/lists/key_functions.lst" "$DXHR_OUT/key_functions.c"
```

Lists in `lists/`:

| list | covers |
|---|---|
| `key_functions.lst` | everything in `ADDRESSES.md`; start here |
| `hot_waits.lst` | wait and sleep sites across the exe (render, worker, file system, jobs, UI) |
| `shaders.lst`, `vertex_shaders.lst` | shader libraries, vertex shader objects and tables, shader manager, input layouts |
| `textures_buffers.lst` | textures, static vertex / index / constant buffers, deferred updates |
| `streaming.lst` | ThreadedFileSystem, MSFileSystem, section loading |

For a quick list, write the addresses to a file yourself, for example `printf '54c800\n54c850\n' > /tmp/mine.lst`.

## 5. Other Ghidra scripts

All take their arguments after the script name, like `Decomp.java` above.

| script | arguments | output |
|---|---|---|
| `Callers.java` | `<VA,VA,...> <out.txt> <depth>` | reverse call tree of each function |
| `Chains.java` | `<VA,VA,...> <out.txt> <depth> <vtables.tsv>` | reverse call tree; references from vtables are named `Class slot N` (make `vtables.tsv` with `helpers/rtti.py`) |
| `Reach.java` | `<roots> <targets> <out.txt> [extra edges a>b,...]` | shortest direct call path from each root to each target |
| `SymSearch.java` | `<regex> <out.txt>` | symbols matching the regex, vtables expanded slot by slot |

## 6. Helpers without Ghidra

These read `DXHR_EXE` directly or the exports in `DXHR_OUT`. Several need a disassembly listing first:

```
objdump -d -M intel "$DXHR_EXE" > "$DXHR_OUT/DXHRDC.objdump.txt"
python3 -I tools/ghidra/helpers/xr.py 54c800      # first run also writes $DXHR_OUT/ins.pkl
```

| helper | use |
|---|---|
| `helpers/pe.py b VA [n]` | raw bytes at a VA (also `dw`, `s`, `dbl`, `flt`, `secs`) |
| `helpers/prol.py VA...` | first bytes of a function and the shortest whole instruction span of at least 5 bytes (what an inline hook overwrites) |
| `helpers/rtti.py > vtables.tsv` | MSVC RTTI vtables with class names |
| `helpers/xr.py VA...` | instructions that reference an address |
| `helpers/ctx.py BEFORE AFTER VA...` | instructions around an address |
| `helpers/vcall.py OFF...` | indirect calls through a given vtable slot offset |
| `helpers/fsuse.py VA` | which vtable slots are called on the object a function returns |
| `helpers/d3dscan.py [window]` | indirect calls that look like D3D11 device or context calls |
| `helpers/xref.py EXE STRING...` | code that references a string (`WIDE=1` for UTF-16) |
| `helpers/callers.sh VA` | direct call and jump sites of an address, with the containing function |
| `helpers/fn.sh VA [size]` | disassembly of one function |
| `helpers/df.sh FILE VA...` | pick functions out of a `Decomp.java` output file |

## 7. Regenerate dist/dxhrfix_syms.tsv

The profiler's symbol file comes from the function list, the exe's RTTI and the hand names in `tools/gen_syms.py`:

```
python3 -I tools/gen_syms.py "$DXHR_OUT/DXHRDC.exe.functions.tsv" "$DXHR_EXE" dist/dxhrfix_syms.tsv
```

With Ghidra 12.1.4 and the supported exe this reproduces the committed file byte for byte. The file holds only addresses, sizes and names (no code).
