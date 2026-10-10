"""Recompile changed translation units, relink copies of the fresh control archive.

The options add no data members or serialized state. All unchanged bitcode,
compiler flags and runtime objects are shared with the control.
"""
import hashlib
import json
import pathlib
import shlex
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/speed"
OUT = ROOT / "build/research"
commands = json.loads((BUILD / "compile_commands.json").read_text())
link = shlex.split((BUILD / "CMakeFiles/melonds_dual.dir/link.txt").read_text())
libs = shlex.split((BUILD / "CMakeFiles/melonds_dual.dir/linkLibs.rsp").read_text())
emdir = pathlib.Path(link[0]).parent
variants = {
    "control": {},
    "late": {"GPU3D_Soft.cpp": "REBIT_MELONDS_LATE_PERSPECTIVE"},
    "indirect": {"ARM.cpp": "REBIT_MELONDS_INDIRECT_DISPATCH"},
    "simd": {"GPU3D_Soft.cpp": "REBIT_MELONDS_SIMD_ATTRIBUTES"},
}
record_path = OUT / "artifacts.json"
records = json.loads(record_path.read_text()) if record_path.exists() else []
for variant, sources in variants.items():
    if len(sys.argv) > 1 and variant not in sys.argv[1:]:
        continue
    out = OUT / variant
    out.mkdir(parents=True, exist_ok=True)
    archive = out / "libcore.a"
    shutil.copy2(BUILD / "melonds/src/libcore.a", archive)
    operations = []
    for source, definition in sources.items():
        entry = next(e for e in commands if pathlib.Path(e["file"]).name == source)
        command = shlex.split(entry["command"])
        obj = out / (source + ".o")
        command[command.index("-o") + 1] = str(obj)
        command.append("-D" + definition + "=1")
        operations.append({"cwd": entry["directory"], "argv": command})
        subprocess.run(command, cwd=entry["directory"], check=True)
        subprocess.run([str(emdir / "emar"), "r", str(archive), str(obj)], check=True)
    command = link.copy()
    command[command.index("-o") + 1] = str(out / "core.js")
    i = command.index("@CMakeFiles/melonds_dual.dir/linkLibs.rsp")
    command[i:i+1] = [str(archive) if arg == "melonds/src/libcore.a" else arg for arg in libs]
    command.append("--emit-symbol-map")
    operations.append({"cwd": str(BUILD), "argv": command})
    (out / "commands.json").write_text(json.dumps(operations, indent=2) + "\n")
    subprocess.run(command, cwd=BUILD, check=True)
    wasm = (out / "core.wasm").read_bytes()
    records = [record for record in records if record["variant"] != variant]
    records.append({"variant": variant, "bytes": len(wasm), "wasmSha256": hashlib.sha256(wasm).hexdigest(),
                    "jsSha256": hashlib.sha256((out / "core.js").read_bytes()).hexdigest()})
    print(json.dumps(records[-1]), flush=True)
    (OUT / "artifacts.json").write_text(json.dumps(records, indent=2) + "\n")
