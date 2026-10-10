#!/usr/bin/env python3
"""Build dist/dxhrfix_syms.tsv for the hitch profiler.

Usage: python3 -I tools/gen_syms.py <DXHRDC.exe.functions.tsv> <stock DXHRDC.exe> <out.tsv>

Output lines: "VA<TAB>name<TAB>size" sorted by VA (VA at image base 0x400000).
Names, best first:
  1. hand names from spec/SPEC.md (KNOWN below);
  2. the Ghidra name when it is not FUN_xxxxxxxx;
  3. Class::vfN from MSVC RTTI vtables (first vtable that holds the function,
     "+k" appended when k more vtables share it);
  4. the Ghidra FUN_ name.
"""
import struct
import sys

KNOWN = {
    0x53A170: "renderFrame",
    0x53BAD0: "renderThreadProc",
    0x53C270: "EndFrame",
    0x53C220: "renderWaitSignal",
    0x53C370: "RenderDevice_ctor",
    0x538D20: "beginFrameDeferredRelease",
    0x538BA0: "flushDeferredReleases",
    0x54C9F0: "RenderContext_present",
    0x54A100: "asyncCreateWorkerProc",
    0x54A000: "asyncEnqueue",
    0x54A060: "asyncCancel",
    0x54A3A0: "dmStartWorker",
    0x54A440: "dmShutdown",
    0x54A930: "createDevice",
    0x54C7D0: "asyncRequest",
    0x54C800: "awaitResource",
    0x54C850: "cancelOrAwait",
    0x54C8A0: "PixelShader_asyncCreate",
    0x553600: "VertexShader_asyncCreate",
    0x52FF00: "BitmapTexture_asyncCreate",
    0x547620: "Texture_asyncCreate",
    0x5307F0: "StaticCB_asyncCreate",
    0x530290: "textureUpload",
    0x530400: "BitmapTexture_get",
    0x530860: "StaticCB_get",
    0x547B50: "BitmapTexture_ctor",
    0x5535F0: "shaderRequest",
    0x54C8D0: "precreateOne",
    0x54C910: "PixelShader_dtor",
    0x54F890: "StateManager_setPixelShader",
    0x54F9C0: "StateManager_setVertexShader",
    0x54F000: "PixelShaderTable_queueUsed",
    0x54F080: "PixelShaderTable_queueUsed4",
    0x54EFA0: "PixelShaderTable_queueAll",
    0x54EE20: "PixelShaderTable_ctor",
    0x54EF40: "PixelShaderTable_dtor",
    0x546EA0: "ShaderLib_ctor",
    0x546E40: "ShaderLib_dtor",
    0x547020: "ShaderLib_fill",
    0x54E570: "ShaderManager_createPixelShader",
    0x54E130: "ShaderManager_unref",
    0x549F10: "deviceResetWalk",
    0x53B5D0: "drawWithTablePS",
    0x628490: "DF_perFrame",
    0x628210: "DF_init",
    0x4A2E10: "ThreadedFileSystem_threadProc",
    0x49FB40: "ThreadedFileSystem_loop",
    0x4A0470: "ThreadedFileSystem_ctor",
    0x6184F0: "renderSetup",
}


def load_funcs(path):
    funcs = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.rstrip("\n").split("\t")
            if len(p) < 2:
                continue
            try:
                va = int(p[0], 16)
            except ValueError:
                continue
            size = int(p[2]) if len(p) > 2 and p[2].isdigit() else 0
            funcs[va] = [p[1].replace("\t", " ").strip() or "FUN_%08x" % va, size]
    return funcs


class PE:
    def __init__(self, data):
        self.d = data
        e = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, e + 6)[0]
        opt = struct.unpack_from("<H", data, e + 20)[0]
        self.base = struct.unpack_from("<I", data, e + 24 + 28)[0]
        self.secs = []
        o = e + 24 + opt
        for i in range(nsec):
            name = data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
            vsize, va, rsize, roff = struct.unpack_from("<IIII", data, o + 8)
            ch = struct.unpack_from("<I", data, o + 36)[0]
            self.secs.append((name, self.base + va, vsize, roff, rsize, ch))
            o += 40

    def off(self, va):
        for _, sva, vs, ro, rs, _ in self.secs:
            if sva <= va < sva + min(vs, rs):
                return ro + (va - sva)
        return None

    def u32(self, va):
        o = self.off(va)
        return None if o is None else struct.unpack_from("<I", self.d, o)[0]

    def is_code(self, va):
        for _, sva, vs, _, _, ch in self.secs:
            if sva <= va < sva + vs:
                return bool(ch & 0x20000020)
        return False


def demangle(raw):
    # ".?AVName@ns1@ns2@@" -> "ns2::ns1::Name" (templates left as is)
    s = raw
    for pre in (".?AV", ".?AU"):
        if s.startswith(pre):
            s = s[len(pre):]
            break
    else:
        return None
    if s.endswith("@@"):
        s = s[:-2]
    if "?$" in s:
        return s.replace("@", "_")
    parts = [p for p in s.split("@") if p]
    return "::".join(reversed(parts)) if parts else None


def rtti_vtables(pe):
    d = pe.d
    # Type descriptors: name string at TD+8.
    tds = {}
    i = d.find(b".?AV")
    while True:
        j = d.find(b".?A", i if i >= 0 else 0)
        if j < 0:
            break
        end = d.find(b"\0", j)
        raw = d[j:end].decode("ascii", "replace")
        name = demangle(raw)
        # map file offset back to VA
        for _, sva, vs, ro, rs, _ in pe.secs:
            if ro <= j - 8 < ro + rs:
                tds[sva + (j - 8 - ro)] = name
                break
        i = end + 1
    # Complete object locators: {0, off, cdOff, pTD, pCHD}
    cols = {}
    for name, sva, vs, ro, rs, ch in pe.secs:
        if ch & 0x20000000:   # skip code
            continue
        buf = d[ro:ro + rs]
        for k in range(0, len(buf) - 20, 4):
            sig, off, cdo, ptd, pchd = struct.unpack_from("<IIIII", buf, k)
            if sig == 0 and ptd in tds and tds[ptd] and off < 0x10000:
                cols[sva + k] = (tds[ptd], off)
    # Vtables: dword == COL address; slots follow.
    out = []
    for name, sva, vs, ro, rs, ch in pe.secs:
        if ch & 0x20000000:
            continue
        buf = d[ro:ro + rs]
        for k in range(0, len(buf) - 8, 4):
            v = struct.unpack_from("<I", buf, k)[0]
            if v in cols:
                cls, off = cols[v]
                vt = sva + k + 4
                slots = []
                n = 0
                while n < 512:
                    f = pe.u32(vt + 4 * n)
                    if f is None or not pe.is_code(f):
                        break
                    if n > 0 and pe.u32(vt + 4 * n - 4 + 4) in cols:
                        break   # next vtable's locator pointer
                    slots.append(f)
                    n += 1
                out.append((cls if off == 0 else "%s@%x" % (cls, off), vt, slots))
    return out



def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    funcs = load_funcs(sys.argv[1])
    pe = PE(open(sys.argv[2], "rb").read())
    if pe.base != 0x400000:
        print("unexpected image base %#x" % pe.base)
        return 1
    vts = rtti_vtables(pe)
    vname = {}
    vcount = {}
    for cls, vt, slots in sorted(vts, key=lambda x: x[1]):
        for n, f in enumerate(slots):
            vcount[f] = vcount.get(f, 0) + 1
            vname.setdefault(f, "%s::vf%d" % (cls, n))
    named_known = named_ghidra = named_rtti = 0
    lines = []
    for va in sorted(set(funcs) | set(KNOWN)):
        g, size = funcs.get(va, ["FUN_%08x" % va, 0])
        if va in KNOWN:
            name = KNOWN[va]
            named_known += 1
        elif not g.startswith("FUN_") and not g.startswith("thunk_FUN_"):
            name = g
            named_ghidra += 1
        elif va in vname:
            name = vname[va]
            if vcount[va] > 1:
                name += "+%d" % (vcount[va] - 1)
            named_rtti += 1
        else:
            name = g
        name = name.replace("\t", " ").replace("\n", " ")
        lines.append("%08x\t%s\t%d\n" % (va, name, size))
    with open(sys.argv[3], "w", newline="\n") as f:
        f.write("# dxhrfix_syms.tsv: DXHRDC.exe (sha256 8266b6b4...ba510a1) function starts.\n")
        f.write("# VA<TAB>name<TAB>size. Generated by tools/gen_syms.py from the Ghidra function list,\n")
        f.write("# MSVC RTTI vtables (Class::vfN) and the names in spec/SPEC.md.\n")
        f.writelines(lines)
    print("%d functions: %d spec names, %d Ghidra names, %d RTTI vtable names, %d vtables"
          % (len(lines), named_known, named_ghidra, named_rtti, len(vts)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
