// Pre-analysis: lay out the flat X-32 group of the ALIS interpreter (Robinson's Requiem DOS),
// apply names (CodeView names from S.TST or transferred names) and seed functions.
// Args: <names.json> <textSize hex> <dataBase hex>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class RRQSetup extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] a = getScriptArgs();
        String namesPath = a[0];
        long textSize = Long.parseLong(a[1], 16);
        long dataBase = Long.parseLong(a[2], 16);
        Memory mem = currentProgram.getMemory();
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        MemoryBlock blk = mem.getBlock(sp.getAddress(0));
        blk.setName("_TEXT");
        mem.split(blk, sp.getAddress(dataBase));
        MemoryBlock t = mem.getBlock(sp.getAddress(0));
        t.setPermissions(true, false, true);
        MemoryBlock d = mem.getBlock(sp.getAddress(dataBase));
        d.setName("_DATA");
        d.setPermissions(true, true, false);
        long end = d.getEnd().getOffset() + 1;
        long bssStart = (end + 0xfff) & ~0xfffL;
        mem.createUninitializedBlock("BSS", sp.getAddress(end), 0x20000, false).setPermissions(true, true, false);

        // tiny JSON reader for [{"addr":N,"name":"..","kind":"..","module":"..","alt":[..]},...]
        String js = new String(Files.readAllBytes(Paths.get(namesPath)), "UTF-8");
        Pattern p = Pattern.compile("\\{[^{}]*?\"addr\":\\s*(\\d+),\\s*\"name\":\\s*\"((?:[^\"\\\\]|\\\\.)*)\",\\s*\"kind\":\\s*\"(\\w+)\"[^{}]*\\}");
        Matcher m = p.matcher(js);
        SymbolTable st = currentProgram.getSymbolTable();
        int n = 0, fn = 0;
        while (m.find()) {
            long addr = Long.parseLong(m.group(1));
            String name = m.group(2).replace("\\\\", "\\").replace("?", "Q").replace("@", "L_");
            String kind = m.group(3);
            Address ad = sp.getAddress(addr);
            if (!mem.contains(ad)) continue;
            try { st.createLabel(ad, name, SourceType.IMPORTED); n++; } catch (Exception e) { }
            boolean local = m.group(2).startsWith("@") || m.group(2).startsWith("?");
            if (kind.equals("code") && !local && addr < textSize) {
                disassemble(ad);
                if (getFunctionAt(ad) == null && createFunction(ad, name) != null) fn++;
            }
        }
        println("RRQSetup: labels " + n + " functions " + fn);
    }
}
