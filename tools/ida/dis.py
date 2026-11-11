import idautils, idc, ida_funcs, ida_auto, os
ida_auto.auto_wait()
with open(os.environ["IW4R_OUT"],"w") as f:
    for e in os.environ["IW4R_EAS"].split(","):
        fn=ida_funcs.get_func(int(e,16))
        f.write("\n===== DIS %x\n"%fn.start_ea)
        for h in idautils.FuncItems(fn.start_ea):
            f.write("%x  %s\n"%(h, idc.GetDisasm(h)))
idc.qexit(0)
