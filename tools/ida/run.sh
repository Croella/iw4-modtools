#!/bin/sh
# Headless IDA runner for reversing the x64 iw4mp loader.
# usage: IW4R_DB=<copy of iw4mp_2013.exe.i64> tools/ida/run.sh <script.py> <out.txt>
#   dec.py   IW4R_EAS=hex,hex IW4R_DEPTH=n   decompile functions (+callees to depth n)
#   dis.py   IW4R_EAS=hex,hex                disassembly (push/pop block args are hidden by Hex-Rays)
#   strx.py  IW4R_PAT=regex                  strings matching regex -> referencing functions
#   dw.py    IW4R_RANGES=hex:count,...       dump dwords (e.g. the block->memtype table at 0x4217f8)
# Always point IW4R_DB at a COPY: IDA rewrites the database it opens.
IDA="${IDA:-/c/Users/Lurkz/Desktop/IDA Professional 9.2/idat.exe}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -n "$IW4R_DB" ] || { echo "set IW4R_DB to a copy of the .i64" >&2; exit 1; }
IW4R_OUT="$2" timeout 900 "$IDA" -A -L"$(dirname "$IW4R_DB")/idalog.txt" -S"$HERE/$1" "$IW4R_DB"
echo exit=$?
