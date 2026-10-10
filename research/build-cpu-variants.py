"""Build matched FIFO-initialized controls and CPU candidates from copied archives."""
import hashlib
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/research"
variants = {"cpu-control": ("speed", False), "cpu-inline": ("cpu-specialized", False),
            "cpu-outline": ("cpu-specialized", True), "cpu-simd": ("cpu-final", False)}
for variant, (source_build, outline) in variants.items():
    if len(sys.argv) > 1 and variant not in sys.argv[1:]: continue
    build = ROOT / "build" / source_build
    out = OUT / variant
    out.mkdir(parents=True, exist_ok=True)
    commands = json.loads((build / "compile_commands.json").read_text())
    link = shlex.split((build / "CMakeFiles/melonds_dual.dir/link.txt").read_text())
    libs = shlex.split((build / "CMakeFiles/melonds_dual.dir/linkLibs.rsp").read_text())
    emdir = pathlib.Path(link[0]).parent
    archive = out / "libcore.a"
    shutil.copy2(build / "melonds/src/libcore.a", archive)
    operations = []
    def compile_source(name, definition, source=None):
        entry = next(e for e in commands if pathlib.Path(e["file"]).name == name)
        command = shlex.split(entry["command"])
        obj = out / (name + ".o")
        command[command.index("-o") + 1] = str(obj)
        if source:
            command[command.index(entry["file"])] = str(source)
            command.extend(["-I", str(source.parent)])
        command.append("-D" + definition + "=1")
        operations.append({"cwd": entry["directory"], "argv": command})
        subprocess.run(command, cwd=entry["directory"], check=True)
        subprocess.run([str(emdir / "emar"), "r", str(archive), str(obj)], check=True)
    compile_source("GPU3D.cpp", "REBIT_MELONDS_ZERO_GPU_FIFO_PADDING")
    if variant == "cpu-simd":
        compile_source("GPU3D_Soft.cpp", "REBIT_MELONDS_SIMD_ATTRIBUTES")
    environment = os.environ.copy()
    if outline:
        generated = out / "generated"
        subprocess.run([sys.executable, str(ROOT / "research/generate-specialized-cpu.py"), str(generated)], check=True)
        for number in [0, 1]:
            filename = f"CPU{number}_Dispatch.cpp"
            compile_source(filename, "REBIT_MELONDS_CPU_OUTLINE", generated / filename)
        real_root = emdir.parent
        adapter = ROOT / "research/wasm-opt-cpu-outline.py"
        adapter.chmod(0o755)
        binaryen = out / "binaryen"
        (binaryen / "bin").mkdir(parents=True, exist_ok=True)
        for tool in (real_root / "bin").iterdir():
            if not (tool.name.startswith("wasm-") or tool.name == "wasm2js"): continue
            target = adapter if tool.name == "wasm-opt" else tool
            dest = binaryen / "bin" / tool.name
            if dest.is_symlink(): assert dest.resolve() == target.resolve()
            else: dest.symlink_to(target)
        environment.update(EM_BINARYEN_ROOT=str(binaryen), REBIT_CPU_REAL_BINARYEN_ROOT=str(real_root))
    command = link.copy()
    command[command.index("-o") + 1] = str(out / "core.js")
    index = command.index("@CMakeFiles/melonds_dual.dir/linkLibs.rsp")
    command[index:index+1] = [str(archive) if arg == "melonds/src/libcore.a" else arg for arg in libs]
    if "--emit-symbol-map" not in command: command.append("--emit-symbol-map")
    operations.append({"cwd": str(build), "argv": command,
                       "environmentOverrides": {key:environment[key] for key in ["EM_BINARYEN_ROOT","REBIT_CPU_REAL_BINARYEN_ROOT"] if key in environment}})
    (out / "commands.json").write_text(json.dumps(operations, indent=2)+"\n")
    subprocess.run(command, cwd=build, env=environment, check=True)
    wasm = (out / "core.wasm").read_bytes()
    record = {"variant":variant,"bytes":len(wasm),"wasmSha256":hashlib.sha256(wasm).hexdigest(),
              "jsSha256":hashlib.sha256((out / "core.js").read_bytes()).hexdigest(),
              "archiveSource":source_build,"zeroGpuFifoPadding":True,"outlinedCpu":outline,
              "simdAttributes":variant=="cpu-simd"}
    path = OUT / "artifacts.json"
    records = [r for r in json.loads(path.read_text()) if r["variant"] != variant]
    path.write_text(json.dumps(records+[record], indent=2)+"\n")
    print(json.dumps(record), flush=True)
