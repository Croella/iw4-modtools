import idc, ida_auto, ida_bytes, os
ida_auto.auto_wait()
with open(os.environ["IW4R_OUT"],"w") as f:
    for spec in os.environ["IW4R_RANGES"].split(","):
        ea,n = spec.split(":"); ea=int(ea,16); n=int(n)
        f.write("%x: %s\n"%(ea," ".join("%x"%ida_bytes.get_dword(ea+4*i) for i in range(n))))
idc.qexit(0)
