// Chains.java <targets comma list> <outfile> <maxdepth> <vtables.tsv>
// Reverse call tree. Data refs from .rdata are resolved to vtable (RTTI class + slot).
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
public class Chains extends GhidraScript {
  PrintWriter pw; FunctionManager fm; ReferenceManager rm;
  TreeMap<Long,String> vt = new TreeMap<>();
  String vtName(long a) {
    Map.Entry<Long,String> e = vt.floorEntry(a);
    if (e == null || a - e.getKey() > 0x400) return null;
    return e.getValue() + " slot " + ((a - e.getKey())/4) + " (vtbl 0x" + Long.toHexString(e.getKey()) + ")";
  }
  void walk(Function f, int depth, int max, Set<Function> path) {
    for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
      Address fa = r.getFromAddress();
      Function c = fm.getFunctionContaining(fa);
      String pad = "  ".repeat(depth);
      if (c == null) {
        String v = vtName(fa.getOffset());
        pw.println(pad + "<- " + fa + " " + r.getReferenceType() + (v != null ? " VTABLE " + v : " (no function)"));
        continue;
      }
      boolean rec = path.contains(c);
      pw.println(pad + "<- " + fa + " " + r.getReferenceType() + " in " + c.getName() + "@" + c.getEntryPoint() + (rec ? " (recursion)" : ""));
      if (!rec && depth < max) { path.add(c); walk(c, depth+1, max, path); path.remove(c); }
    }
  }
  public void run() throws Exception {
    String[] a = getScriptArgs();
    fm = currentProgram.getFunctionManager(); rm = currentProgram.getReferenceManager();
    int max = Integer.parseInt(a[2]);
    for (String l : Files.readAllLines(Paths.get(a[3]))) {
      String[] p = l.split("\t"); vt.put(Long.parseLong(p[0],16), p[1] + p[2]);
    }
    pw = new PrintWriter(new FileWriter(a[1]));
    for (String s : a[0].split(",")) {
      Function f = fm.getFunctionContaining(toAddr(s));
      pw.println("== " + s + " " + (f==null?"?":f.getName()+"@"+f.getEntryPoint()));
      if (f != null) { Set<Function> p = new HashSet<>(); p.add(f); walk(f, 1, max, p); }
      pw.flush();
    }
    pw.close();
  }
}
