# Script builtins: for each name in IW4R_NAMES (comma list), finds the string, the table entries
# that point at it ({const char* name; void (*fn)(); int developer} rows), and decompiles the
# function next to each (+ callees to IW4R_DEPTH).
import idautils, idc, ida_funcs, ida_auto, ida_hexrays, ida_bytes, os
ida_auto.auto_wait()
ok = ida_hexrays.init_hexrays_plugin()
names = os.environ["IW4R_NAMES"].split(",")
depth = int(os.environ.get("IW4R_DEPTH", "0"))
strs = {}
for s in idautils.Strings():
    t = str(s)
    if t in names: strs.setdefault(t, []).append(s.ea)
seen = set()
def dec(fn, d, f):
    if fn in seen: return
    seen.add(fn)
    try:
        c = ida_hexrays.decompile(fn)
        f.write("\n----- %x (depth %d)\n%s\n" % (fn, d, str(c)))
    except Exception as ex:
        f.write("\n----- %x decompile failed %s\n" % (fn, ex)); return
    if d >= depth: return
    for h in idautils.FuncItems(fn):
        if idc.print_insn_mnem(h) == "call":
            t = idc.get_operand_value(h, 0)
            g = ida_funcs.get_func(t)
            if g and g.start_ea == t and (g.end_ea - g.start_ea) < 6000: dec(t, d + 1, f)
with open(os.environ["IW4R_OUT"], "w", encoding="utf-8") as f:
    for n in names:
        for ea in strs.get(n, []):
            for r in idautils.DataRefsTo(ea):
                fnp = ida_bytes.get_qword(r + 8)
                f.write("== %s str %x row %x fn %x\n" % (n, ea, r, fnp))
                if ida_funcs.get_func(fnp): dec(fnp, 0, f)
idc.qexit(0)
