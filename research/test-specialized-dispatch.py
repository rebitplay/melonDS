"""Compile the generated dispatchers to WASM and check every canonical opcode.

Handlers are recording stubs: this checks routing and CPU selection, not ISA
semantics. Full-core state/recovery comparisons cover the integration separately.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/research/specialized-dispatch-test"
GENERATED = OUT / "generated"
OUT.mkdir(parents=True, exist_ok=True)
subprocess.run([sys.executable, str(ROOT / "research/generate-specialized-cpu.py"), str(GENERATED)], check=True)
source = (ROOT / "src/ARM_InstrTable.h").read_text()
source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.DOTALL)
tables = {}
for name, size in [("ARM", 4096), ("THUMB", 1024)]:
    body = re.search(rf"{name}InstrTable\[{size}\]\)\s*=\s*\{{(.*?)\}};", source, re.DOTALL).group(1)
    tables[name] = [entry.strip() for entry in body.split(",") if entry.strip()]
    assert len(tables[name]) == size
handlers = sorted(set(tables["ARM"] + tables["THUMB"] + ["A_BLX_IMM"]))
ids = {handler: i for i, handler in enumerate(handlers)}
header = '''#pragma once
#include <cstdint>
#include <cassert>
using u16=uint16_t; using u32=uint32_t;
namespace melonDS {
struct ARMv5 { u32 selected=~0u; int observed=-1; };
struct ARMv4 { u32 selected=~0u; int observed=-1; };
}
'''
for number, cpu_type in [(0, "ARMv5"), (1, "ARMv4")]:
    header += f"namespace melonDS::CPU{number}Interpreter {{\n"
    for name in handlers:
        header += f"inline void {name}({cpu_type}* cpu) {{ cpu->selected={ids[name]}; cpu->observed={number}; }}\n"
    header += "}\n"
(OUT / "CPUInterpreters.h").write_text(header)
for number in [0, 1]:
    # Keep dispatcher bytes unchanged; only its include resolves to the stubs.
    (OUT / f"CPU{number}_Dispatch.cpp").write_bytes((GENERATED / f"CPU{number}_Dispatch.cpp").read_bytes())
test = '#include <cstdio>\n#include "CPU0_Dispatch.cpp"\n#include "CPU1_Dispatch.cpp"\nint main() {\n'
for name, entries in tables.items():
    test += f"const u32 {name}Expected[]={{" + ",".join(str(ids[h]) for h in entries) + "};\n"
    for number, cpu_type in [(0, "ARMv5"), (1, "ARMv4")]:
        test += f"for(u32 code=0;code<{len(entries)};++code) {{ melonDS::{cpu_type} cpu; melonDS::CPU{number}Interpreter::Dispatch{name}(&cpu,code); assert(cpu.selected=={name}Expected[code]); assert(cpu.observed=={number}); }}\n"
test += 'std::puts("PASS: 10240 compiled WASM opcode/CPU routing checks"); }\n'
(OUT / "test.cpp").write_text(test)
subprocess.run(["/home/daudau/Code/emsdk/upstream/emscripten/em++", "-O3", "-std=c++17",
                "-sENVIRONMENT=node", "-sSINGLE_FILE=1", str(OUT / "test.cpp"),
                "-o", str(OUT / "test.cjs")], check=True)
subprocess.run(["node", str(OUT / "test.cjs")], check=True)
