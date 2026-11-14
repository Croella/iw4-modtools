# Prints the functions that call each of IW4R_EAS (hex), up to IW4R_DEPTH levels, to IW4R_OUT.
import idautils, idc, ida_funcs, ida_auto, os
ida_auto.auto_wait()
out = open(os.environ["IW4R_OUT"], "w")
depth = int(os.environ.get("IW4R_DEPTH", "1"))
def walk(ea, d, ind):
    if d > depth: return
    callers = sorted({ida_funcs.get_func(x).start_ea for x in idautils.CodeRefsTo(ea, 0) if ida_funcs.get_func(x)})
    for c in callers:
        out.write("%s%x (size %x)\n" % ("  " * ind, c, ida_funcs.get_func(c).end_ea - c))
        walk(c, d + 1, ind + 1)
for r in os.environ["IW4R_EAS"].split(","):
    out.write("== callers of %s\n" % r)
    walk(int(r, 16), 1, 1)
out.close()
idc.qexit(0)
