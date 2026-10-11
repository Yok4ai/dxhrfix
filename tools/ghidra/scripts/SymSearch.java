import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.util.regex.*;
public class SymSearch extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    Pattern p = Pattern.compile(a[0]);
    FunctionManager fm = currentProgram.getFunctionManager();
    try (PrintWriter pw = new PrintWriter(new FileWriter(a[1]))) {
      for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
        String n = s.getName(true);
        if (!p.matcher(n).find()) continue;
        pw.println(s.getAddress() + "\t" + n);
        if (n.contains("vftable")) {
          Address ad = s.getAddress();
          for (int i = 0; i < 80; i++) {
            Address e = ad.add(i*4);
            if (i>0 && currentProgram.getSymbolTable().getPrimarySymbol(e)!=null && currentProgram.getSymbolTable().getPrimarySymbol(e).getName().contains("vftable")) break;
            int v = getInt(e);
            Address t = toAddr(v & 0xffffffffL);
            Function f = fm.getFunctionAt(t);
            if (f == null && !currentProgram.getMemory().contains(t)) break;
            pw.println("  [" + i + "] +0x" + Integer.toHexString(i*4) + " " + t + " " + (f==null?"":f.getName()));
          }
        }
      }
    }
  }
}
