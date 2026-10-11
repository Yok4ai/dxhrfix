// Reach.java <roots csv> <targets csv> <outfile> [extra edges csv "a>b"]
// Forward BFS over direct call references from each root; prints shortest path root->target.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.util.*;
public class Reach extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    FunctionManager fm = currentProgram.getFunctionManager();
    ReferenceManager rm = currentProgram.getReferenceManager();
    Map<Address,List<Address>> extra = new HashMap<>();
    if (a.length > 3) for (String e : a[3].split(",")) {
      String[] p = e.split(">"); Function f1 = fm.getFunctionContaining(toAddr(p[0])), f2 = fm.getFunctionContaining(toAddr(p[1]));
      if (f1 != null && f2 != null) extra.computeIfAbsent(f1.getEntryPoint(), k -> new ArrayList<>()).add(f2.getEntryPoint());
    }
    PrintWriter pw = new PrintWriter(new FileWriter(a[2]));
    List<Address> targets = new ArrayList<>();
    for (String t : a[1].split(",")) { Function f = fm.getFunctionContaining(toAddr(t)); if (f != null) targets.add(f.getEntryPoint()); else pw.println("# no function for target " + t); }
    for (String r : a[0].split(",")) {
      Function rf = fm.getFunctionContaining(toAddr(r));
      if (rf == null) { pw.println("# no root " + r); continue; }
      Map<Address,Address> parent = new HashMap<>();
      Map<Address,Address> site = new HashMap<>();
      ArrayDeque<Address> q = new ArrayDeque<>();
      parent.put(rf.getEntryPoint(), null); q.add(rf.getEntryPoint());
      while (!q.isEmpty()) {
        Address cur = q.poll(); Function f = fm.getFunctionAt(cur); if (f == null) continue;
        List<Address[]> outs = new ArrayList<>();
        for (Address ad : f.getBody().getAddresses(true)) {
          for (Reference ref : rm.getReferencesFrom(ad)) {
            if (!ref.getReferenceType().isCall() && !(ref.getReferenceType().isJump() && fm.getFunctionAt(ref.getToAddress()) != null && !f.getBody().contains(ref.getToAddress()))) continue;
            Function g = fm.getFunctionAt(ref.getToAddress());
            if (g == null) { g = fm.getFunctionContaining(ref.getToAddress()); if (g == null) continue; }
            if (g.isThunk()) { Function tg = g.getThunkedFunction(true); if (tg != null && !tg.isExternal()) g = tg; }
            outs.add(new Address[]{g.getEntryPoint(), ad});
          }
        }
        for (Address e : extra.getOrDefault(cur, Collections.emptyList())) outs.add(new Address[]{e, cur});
        for (Address[] o : outs) if (!parent.containsKey(o[0])) { parent.put(o[0], cur); site.put(o[0], o[1]); q.add(o[0]); }
      }
      pw.println("== root " + rf.getName() + "@" + rf.getEntryPoint() + " reaches " + parent.size() + " functions");
      for (Address t : targets) {
        if (!parent.containsKey(t)) { pw.println("  " + t + " NOT reached"); continue; }
        List<String> path = new ArrayList<>();
        for (Address x = t; x != null; x = parent.get(x)) { Function fx = fm.getFunctionAt(x); path.add(0, (fx==null?"?":fx.getName()) + (site.get(x)!=null? "[via " + site.get(x) + "]" : "")); }
        pw.println("  " + t + " depth " + (path.size()-1) + ": " + String.join(" -> ", path));
      }
      pw.flush();
    }
    pw.close();
  }
}
