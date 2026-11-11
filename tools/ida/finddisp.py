# Lists functions whose memory operands use ALL of the given displacements (struct field
# offsets), to find the code that reads a struct without symbols. Optional decompile of hits.
#   IW4R_DISPS=0x11c,0x120,0x128   IW4R_DECOMP=1   IW4R_MAXHITS=8
import idautils, idc, ida_funcs, ida_auto, ida_ua, ida_hexrays, os
ida_auto.auto_wait()
want = set(int(x, 16) for x in os.environ["IW4R_DISPS"].split(","))
decomp = os.environ.get("IW4R_DECOMP", "1") == "1"
maxhits = int(os.environ.get("IW4R_MAXHITS", "8"))
hits = []
for fea in idautils.Functions():
    fn = ida_funcs.get_func(fea)
    if fn.end_ea - fn.start_ea > 20000: continue
    found = set()
    for h in idautils.FuncItems(fea):
        insn = ida_ua.insn_t()
        if not ida_ua.decode_insn(insn, h): continue
        for op in insn.ops:
            if op.type in (ida_ua.o_displ, ida_ua.o_phrase) and op.addr in want:
                found.add(op.addr)
    if found == want:
        hits.append(fea)
with open(os.environ["IW4R_OUT"], "w", encoding="utf-8") as f:
    f.write("hits: %s\n" % ", ".join("%x" % x for x in hits))
    if decomp:
        ida_hexrays.init_hexrays_plugin()
        for ea in hits[:maxhits]:
            try: c = str(ida_hexrays.decompile(ea))
            except Exception as e: c = "failed %s" % e
            f.write("\n===== FUNC %x\n%s\n" % (ea, c))
idc.qexit(0)
