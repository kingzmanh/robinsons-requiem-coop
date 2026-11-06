// Post-analysis export: functions.tsv, calls.tsv, strings.tsv, and decompiled C for every function
// (decomp/<prefix>_all.c). Args: <outdir> <prefix>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;
import java.util.*;

public class RRQExport extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs()[0];
        String pre = getScriptArgs()[1];
        new File(out).mkdirs();
        Listing listing = currentProgram.getListing();
        ReferenceManager refs = currentProgram.getReferenceManager();

        Map<Address, String> strings = new TreeMap<>();
        for (Data d : listing.getDefinedData(true)) {
            StringDataInstance s = StringDataInstance.getStringDataInstance(d);
            if (s != StringDataInstance.NULL_INSTANCE) {
                String v = s.getStringValue();
                if (v != null && v.length() >= 3) strings.put(d.getAddress(), v.replaceAll("[\\t\\r\\n]", " "));
            }
        }
        try (PrintWriter w = new PrintWriter(new FileWriter(new File(out, pre + "_strings.tsv")))) {
            w.println("addr\tstring");
            for (Map.Entry<Address, String> e : strings.entrySet()) w.printf("%s\t%s%n", e.getKey(), e.getValue());
        }

        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        int nf = 0;
        try (PrintWriter fw = new PrintWriter(new FileWriter(new File(out, pre + "_functions.tsv")));
             PrintWriter cw = new PrintWriter(new FileWriter(new File(out, pre + "_calls.tsv")));
             PrintWriter dw = new PrintWriter(new FileWriter(new File(out, pre + "_all.c")))) {
            fw.println("entry\tname\tsize\tcallers\tcallees\tstrings\tdata_refs");
            cw.println("caller\tcallee\tcaller_name\tcallee_name");
            for (Function f : listing.getFunctions(true)) {
                if (monitor.isCancelled()) break;
                Set<Function> callees = f.getCalledFunctions(monitor);
                Set<Function> callers = f.getCallingFunctions(monitor);
                for (Function c : callees)
                    cw.printf("%s\t%s\t%s\t%s%n", f.getEntryPoint(), c.getEntryPoint(), f.getName(), c.getName());
                StringBuilder sb = new StringBuilder();
                TreeSet<String> dref = new TreeSet<>();
                for (Address a : f.getBody().getAddresses(true)) {
                    for (Reference r : refs.getReferencesFrom(a)) {
                        Address to = r.getToAddress();
                        String s = strings.get(to);
                        if (s != null) { if (sb.length() > 0) sb.append(" | "); sb.append(s); }
                        else if (r.getReferenceType().isData()) {
                            Symbol sym = currentProgram.getSymbolTable().getPrimarySymbol(to);
                            if (sym != null && sym.getSource() != SourceType.DEFAULT) dref.add(sym.getName());
                        }
                    }
                }
                fw.printf("%s\t%s\t%d\t%d\t%d\t%s\t%s%n", f.getEntryPoint(), f.getName(), f.getBody().getNumAddresses(),
                        callers.size(), callees.size(), sb, String.join(",", dref));
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                dw.println("// ==== " + f.getName() + " @ " + f.getEntryPoint() + " size " + f.getBody().getNumAddresses());
                if (r != null && r.decompileCompleted()) dw.println(r.getDecompiledFunction().getC());
                else dw.println("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
                nf++;
            }
        }
        di.dispose();
        println("RRQExport: " + nf + " functions -> " + out);
    }
}
