# Decompiles functions (+ callees to IW4R_DEPTH) and annotates every DB_PushStreamPos call
# (sub_132140, whose block argument Hex-Rays drops) with the block index taken from the
# disassembly: "sub_132140()" becomes "PUSH(3)". Also renames the known stream helpers.
#   IW4R_EAS=hex,hex IW4R_DEPTH=n IW4R_MAXSZ=bytes
import idautils, idc, ida_funcs, ida_auto, ida_hexrays, ida_ua, os, re
ida_auto.auto_wait()
ida_hexrays.init_hexrays_plugin()
out = os.environ["IW4R_OUT"]
roots = [int(x, 16) for x in os.environ["IW4R_EAS"].split(",")]
depth = int(os.environ.get("IW4R_DEPTH", "1"))
maxsz = int(os.environ.get("IW4R_MAXSZ", "100000"))
PUSH = 0x132140
NAMES = {"sub_132140": "PUSH", "sub_132100": "POP()", "sub_131FD0": "ALLOC", "sub_1322A0": "LOAD_STREAM",
         "sub_132080": "INSERT_PTR", "sub_132270": "CONVERT_OFFSET", "sub_132240": "CONVERT_INSERTED",
         "sub_132380": "LOAD_XSTRING_DATA", "sub_1322F0": "LOAD_SCRIPTSTRING_DATA"}
skip = {0x1322A0, 0x132140, 0x132100, 0x131FD0, 0x132080, 0x132270, 0x132240, 0x132380, 0x1322F0,
        0x131FF0, 0x132190, 0x1182D0, 0x327810, 0x327FA0}

def push_args(fn):
    args = []
    last = None
    for h in idautils.FuncItems(fn.start_ea):
        m = idc.print_insn_mnem(h)
        if m in ("mov", "xor") and idc.print_operand(h, 0) in ("ecx", "cl", "rcx"):
            if m == "xor": last = "0"
            else: last = idc.print_operand(h, 1).rstrip("h")
        if m == "call" and idc.get_operand_value(h, 0) == PUSH:
            args.append(last if last is not None else "?")
            last = None
        elif m == "call":
            last = None
    return args

seen = set(); order = []
def walk(ea, d):
    fn = ida_funcs.get_func(ea)
    if not fn or fn.start_ea in seen or fn.start_ea in skip: return
    seen.add(fn.start_ea); order.append((fn.start_ea, d))
    if d >= depth: return
    for h in idautils.FuncItems(fn.start_ea):
        for x in idautils.CodeRefsFrom(h, 0):
            f2 = ida_funcs.get_func(x)
            if f2 and f2.start_ea != fn.start_ea and (f2.end_ea - f2.start_ea) < maxsz: walk(f2.start_ea, d + 1)
for r in roots: walk(r, 0)

with open(out, "w", encoding="utf-8") as f:
    for ea, d in order:
        fn = ida_funcs.get_func(ea)
        try: c = str(ida_hexrays.decompile(ea))
        except Exception as e: c = "decompile failed %s" % e
        args = iter(push_args(fn))
        c = re.sub(r"sub_132140\([^)]*\)", lambda m: "PUSH(%s)" % next(args, "?"), c)
        for k, v in NAMES.items():
            if k != "sub_132140": c = c.replace(k + "()" if v.endswith("()") else k, v)
        f.write("\n===== FUNC %x depth %d\n%s\n" % (ea, d, c))
idc.qexit(0)
