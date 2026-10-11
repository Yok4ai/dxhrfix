// Export function list, string xrefs, import callers
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.*;
import java.io.*;
import java.util.*;

public class ExportBasics extends GhidraScript {
  public void run() throws Exception {
    String outDir = getScriptArgs()[0];
    String base = currentProgram.getName();
    FunctionManager fm = currentProgram.getFunctionManager();
    try (PrintWriter pw = new PrintWriter(new FileWriter(outDir + "/" + base + ".functions.tsv"))) {
      for (Function f : fm.getFunctions(true)) {
        pw.println(f.getEntryPoint() + "\t" + f.getName(true) + "\t" + f.getBody().getNumAddresses());
      }
    }
    ReferenceManager rm = currentProgram.getReferenceManager();
    try (PrintWriter pw = new PrintWriter(new FileWriter(outDir + "/" + base + ".strings_xrefs.tsv"))) {
      DataIterator it = currentProgram.getListing().getDefinedData(true);
      while (it.hasNext() && !monitor.isCancelled()) {
        Data d = it.next();
        if (!d.hasStringValue()) continue;
        String s = String.valueOf(d.getValue()).replace("\n","\\n").replace("\t","\\t");
        StringBuilder sb = new StringBuilder();
        for (Reference r : rm.getReferencesTo(d.getAddress())) {
          Function f = fm.getFunctionContaining(r.getFromAddress());
          sb.append(r.getFromAddress()).append("@").append(f==null?"?":f.getName()+"/"+f.getEntryPoint()).append(" ");
        }
        pw.println(d.getAddress() + "\t" + s + "\t" + sb);
      }
    }
    try (PrintWriter pw = new PrintWriter(new FileWriter(outDir + "/" + base + ".imports_callers.tsv"))) {
      SymbolTable st = currentProgram.getSymbolTable();
      for (Symbol sym : st.getExternalSymbols()) {
        StringBuilder sb = new StringBuilder();
        Set<Address> seen = new HashSet<>();
        for (Reference r : sym.getReferences()) {
          // refs to external are usually from thunk/IAT pointer; follow refs to IAT addr
          Address from = r.getFromAddress();
          Function f = fm.getFunctionContaining(from);
          sb.append(from).append("@").append(f==null?"?":f.getName()).append(" ");
          for (Reference r2 : rm.getReferencesTo(from)) {
            Function f2 = fm.getFunctionContaining(r2.getFromAddress());
            sb.append(r2.getFromAddress()).append("@").append(f2==null?"?":f2.getName()+"/"+f2.getEntryPoint()).append(" ");
          }
        }
        pw.println(sym.getParentNamespace().getName() + "!" + sym.getName() + "\t" + sb);
      }
    }
    println("done export " + base);
  }
}
