import idautils, idc, ida_funcs, ida_auto, ida_hexrays, os
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
out = os.environ["IW4R_OUT"]
roots = [int(x,16) for x in os.environ["IW4R_EAS"].split(",")]
depth = int(os.environ.get("IW4R_DEPTH","1"))
maxsz = int(os.environ.get("IW4R_MAXSZ","4000"))
seen=set(); order=[]
def walk(ea,d):
    fn=ida_funcs.get_func(ea)
    if not fn or fn.start_ea in seen: return
    seen.add(fn.start_ea); order.append((fn.start_ea,d))
    if d>=depth: return
    for h in idautils.FuncItems(fn.start_ea):
        for x in idautils.CodeRefsFrom(h,0):
            f2=ida_funcs.get_func(x)
            if f2 and f2.start_ea!=fn.start_ea and (f2.end_ea-f2.start_ea)<maxsz: walk(f2.start_ea,d+1)
for r in roots: walk(r,0)
with open(out,"w",encoding="utf-8") as f:
    for ea,d in order:
        try: c=str(ida_hexrays.decompile(ea))
        except Exception as e: c="decompile failed %s"%e
        f.write("\n===== FUNC %x depth %d\n%s\n"%(ea,d,c))
idc.qexit(0)
