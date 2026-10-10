"""Generate typed copies of existing interpreters without changing algorithms.

Outputs live in the build directory. All opcode mappings come from the same
canonical table as the normal dispatcher. No instruction or data cache is added.
"""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
SOURCES = ["ARMInterpreter.cpp", "ARMInterpreter_ALU.cpp",
           "ARMInterpreter_LoadStore.cpp", "ARMInterpreter_Branch.cpp"]
HELPERS = {name: name for name in ["DataRead8", "DataRead16", "DataRead32", "DataRead32S",
                                  "DataWrite8", "DataWrite16", "DataWrite32", "DataWrite32S", "JumpTo"]}
HELPERS.update(AddCyclesC="AddCycles_C", AddCyclesCI="AddCycles_CI",
               AddCyclesCD="AddCycles_CD", AddCyclesCDI="AddCycles_CDI")
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location("dispatch", ROOT / "rebit/emscripten/generate_arm_dispatch.py")
dispatch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dispatch)


def specialize(source, number, cpu_type):
    assert source.count("namespace melonDS::ARMInterpreter") == 1
    # The system source also owns the generic tables/dispatcher. Typed copies
    # get their dispatcher in a separate generated translation unit instead.
    if '#include "ARM_InstrTable.h"' in source:
        source, count = re.subn(r'#define INSTRFUNC_PROTO\(x\).*?\n#undef INSTRFUNC_PROTO', '', source, flags=re.DOTALL)
        assert count == 1
        source, count = re.subn(r'#ifdef __EMSCRIPTEN__\n#include "ARM_InstrDispatch.h"\n#endif', '', source)
        assert count == 1
    result = source.replace("namespace melonDS::ARMInterpreter", f"namespace melonDS::CPU{number}Interpreter")
    result = result.replace("ARM* cpu", f"{cpu_type}* cpu").replace("cpu->Num", str(number))
    for helper, method in HELPERS.items():
        result = re.sub(rf"Fast::{helper}\(cpu(?:,\s*)?", f"cpu->{cpu_type}::{method}(", result)
    # A few non-sequential block-transfer calls still used virtual dispatch.
    methods = "|".join(HELPERS.values())
    result = re.sub(rf"cpu->({methods})\s*\(", rf"cpu->{cpu_type}::\1(", result)
    assert not re.search(r"Fast::|ARM\* cpu|cpu->Num", result)
    assert not re.search(rf"cpu->({methods})\s*\(", result)
    return '#include "CPUInterpreters.h"\n' + result


def generate(output):
    output.mkdir(parents=True, exist_ok=True)
    table = (ROOT / "src/ARM_InstrTable.h").read_text()
    tables = {"ARM": dispatch.parse_table(table, "ARMInstrTable", 4096),
              "THUMB": dispatch.parse_table(table, "THUMBInstrTable", 1024)}
    handlers = sorted(set(tables["ARM"] + tables["THUMB"] + ["A_BLX_IMM"]))
    header = '#pragma once\n#include "ARM.h"\n'
    records = []
    for number, cpu_type in [(0, "ARMv5"), (1, "ARMv4")]:
        namespace = f"melonDS::CPU{number}Interpreter"
        header += f"namespace {namespace} {{\n"
        header += "\n".join(f"void {name}({cpu_type}* cpu);" for name in handlers)
        header += f"\nvoid DispatchARM({cpu_type}* cpu, u32 code);\nvoid DispatchTHUMB({cpu_type}* cpu, u32 code);\n}}\n"
        for filename in SOURCES:
            original = (ROOT / "src" / filename).read_text()
            generated = specialize(original, number, cpu_type)
            name = f"CPU{number}_{filename}"
            (output / name).write_text(generated)
            records.append({"source": filename, "output": name,
                            "sourceSha256": hashlib.sha256(original.encode()).hexdigest(),
                            "outputSha256": hashlib.sha256(generated.encode()).hexdigest()})
        generated = '#include "CPUInterpreters.h"\n' + f"namespace {namespace} {{\n"
        for mode, entries in tables.items():
            generated += '#ifdef REBIT_MELONDS_CPU_OUTLINE\n[[gnu::noinline]]\n#endif\n'
            generated += dispatch.render_dispatch("Dispatch" + mode, entries).replace("ARM* cpu", f"{cpu_type}* cpu") + "\n"
        generated += "}\n"
        (output / f"CPU{number}_Dispatch.cpp").write_text(generated)
    (output / "CPUInterpreters.h").write_text(header)
    (output / "generation.json").write_text(json.dumps({"files": records, "tables": tables}, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=pathlib.Path)
    generate(parser.parse_args().output)
