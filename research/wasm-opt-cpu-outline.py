#!/usr/bin/env python3
"""Build-local adapter: keep typed dispatchers out of the frame function."""
import os
import pathlib
import subprocess
import sys


def function_names(data):
    offset = 8
    def leb():
        nonlocal offset
        result = shift = 0
        while True:
            value = data[offset]; offset += 1
            result |= (value & 127) << shift
            if not value & 128: return result
            shift += 7
    def string():
        nonlocal offset
        size = leb(); result = data[offset:offset+size].decode(); offset += size
        return result
    names = []
    while offset < len(data):
        section = data[offset]; offset += 1
        size = leb(); end = offset + size
        if section == 0 and string() == "name":
            while offset < end:
                sub = data[offset]; offset += 1
                length = leb(); sub_end = offset + length
                if sub == 1:
                    for _ in range(leb()):
                        leb(); names.append(string())
                offset = sub_end
        offset = end
    return names


args = sys.argv[1:]
real = pathlib.Path(os.environ["REBIT_CPU_REAL_BINARYEN_ROOT"]) / "bin/wasm-opt"
if "-O3" in args:
    file = next(pathlib.Path(arg) for arg in args if arg.endswith(".wasm") and not arg.startswith("-"))
    names = [name for name in function_names(file.read_bytes())
             if ("CPU0Interpreter" in name or "CPU1Interpreter" in name) and "Dispatch" in name]
    assert len(names) == 4, f"Expected four typed dispatchers before optimization, got {names}"
    print("Keeping all four typed CPU dispatchers separate during Binaryen optimization", file=sys.stderr)
if not ("--version" in args or "--help" in args):
    args = ["--no-inline=*CPU0Interpreter*Dispatch*", "--no-inline=*CPU1Interpreter*Dispatch*", *args]
sys.exit(subprocess.run([str(real), *args]).returncode)
