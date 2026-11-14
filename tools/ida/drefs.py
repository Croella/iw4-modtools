# Functions that reference each of IW4R_EAS (hex data addresses), decompiled (IW4R_DECOMP=1) to IW4R_OUT.
import idautils, idc, ida_funcs, ida_auto, ida_hexrays, os
ida_auto.auto_wait()
ok = ida_hexrays.init_hexrays_plugin()
dec = os.environ.get("IW4R_DECOMP", "1") == "1"
seen = set()
with open(os.environ["IW4R_OUT"], "w", encoding="utf-8") as f:
    for e in os.environ["IW4R_EAS"].split(","):
        ea = int(e, 16)
        fs = sorted({ida_funcs.get_func(x).start_ea for x in idautils.DataRefsTo(ea) if ida_funcs.get_func(x)})
        f.write("== refs to %x: %s\n" % (ea, " ".join("%x" % x for x in fs)))
        for fn in fs:
            if fn in seen or not dec: continue
            seen.add(fn)
            try:
                f.write("\n----- %x\n%s\n" % (fn, str(ida_hexrays.decompile(fn))))
            except Exception as ex:
                f.write("\n----- %x decompile failed %s\n" % (fn, ex))
idc.qexit(0)
