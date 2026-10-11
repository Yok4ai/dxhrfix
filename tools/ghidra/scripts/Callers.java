import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.util.*;
public class Callers extends GhidraScript {
  PrintWriter pw; FunctionManager fm; ReferenceManager rm;
  void walk(Function f, int depth, int max, Set<Function> seen) {
    for (Reference r : rm.getReferencesTo(f.getEntryPoint())) {
      Function c = fm.getFunctionContaining(r.getFromAddress());
      String pad = "  ".repeat(depth);
      String nm = c==null? "?" : c.getName()+"@"+c.getEntryPoint();
      pw.println(pad + "<- " + r.getFromAddress() + " " + r.getReferenceType() + " in " + nm);
      if (c != null && depth < max && seen.add(c)) walk(c, depth+1, max, seen);
    }
  }
  public void run() throws Exception {
    String[] a = getScriptArgs();
    fm = currentProgram.getFunctionManager(); rm = currentProgram.getReferenceManager();
    int max = Integer.parseInt(a[2]);
    pw = new PrintWriter(new FileWriter(a[1]));
    for (String s : a[0].split(",")) {
      Function f = fm.getFunctionContaining(toAddr(s));
      pw.println("== " + s + " " + (f==null?"?":f.getName()));
      if (f != null) walk(f, 1, max, new HashSet<>());
    }
    pw.close();
  }
}
