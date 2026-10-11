// Decompile functions listed in a file (one addr per line, optional label after tab) to outfile
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;

public class Decomp extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    List<String> lines = Files.readAllLines(Paths.get(a[0]));
    DecompInterface di = new DecompInterface();
    di.openProgram(currentProgram);
    FunctionManager fm = currentProgram.getFunctionManager();
    try (PrintWriter pw = new PrintWriter(new FileWriter(a[1]))) {
      for (String ln : lines) {
        ln = ln.trim(); if (ln.isEmpty() || ln.startsWith("#")) continue;
        String[] p = ln.split("\t");
        Address ad = toAddr(p[0]);
        Function f = fm.getFunctionContaining(ad);
        if (f == null) { pw.println("// no function at " + p[0]); continue; }
        DecompileResults r = di.decompileFunction(f, 120, monitor);
        pw.println("// ===== " + f.getEntryPoint() + " " + f.getName(true) + (p.length>1? "  ["+p[1]+"]":""));
        if (r.decompileCompleted()) pw.println(r.getDecompiledFunction().getC());
        else pw.println("// decompile failed: " + r.getErrorMessage());
        pw.flush();
      }
    }
  }
}
