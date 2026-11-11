import idautils, idc, ida_funcs, ida_auto, ida_hexrays, re, os
ida_auto.auto_wait()
out = os.environ["IW4R_OUT"]
pat = re.compile(os.environ["IW4R_PAT"], re.I)
decomp = os.environ.get("IW4R_DECOMP","1")=="1"
ok = ida_hexrays.init_hexrays_plugin()
funcs = {}
with open(out, "w", encoding="utf-8") as f:
    f.write("hexrays=%s imagebase=%x\n" % (ok, idc.get_inf_attr(idc.INF_MIN_EA)))
    for s in idautils.Strings():
        t = str(s)
        if not pat.search(t): continue
        refs = [x.frm for x in idautils.XrefsTo(s.ea)]
        fs = []
        for r in refs:
            fn = ida_funcs.get_func(r)
            if fn:
                fs.append(fn.start_ea); funcs.setdefault(fn.start_ea, set()).add(t)
        f.write("STR %x %r -> %s\n" % (s.ea, t, ",".join("%x"%x for x in fs)))
    if decomp and ok:
        for ea in sorted(funcs):
            try:
                c = ida_hexrays.decompile(ea)
                f.write("\n===== FUNC %x  (%s)\n%s\n" % (ea, "; ".join(sorted(funcs[ea]))[:200], str(c)))
            except Exception as e:
                f.write("\n===== FUNC %x decompile failed %s\n" % (ea, e))
idc.qexit(0)
