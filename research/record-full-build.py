"""Copy the full CMake candidate to the benchmark matrix and record its hashes."""
import hashlib
import json
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/research"
variant = sys.argv[1] if len(sys.argv) > 1 else "simd-full"
destination = OUT / variant
destination.mkdir(parents=True, exist_ok=True)
for suffix in ["js", "wasm"]:
    shutil.copy2(ROOT / f"build/{variant}/dist/melonds_dual.{suffix}", destination / f"core.{suffix}")
symbols = ROOT / f"build/{variant}/dist/melonds_dual.js.symbols"
if symbols.exists():
    shutil.copy2(symbols, destination / "core.js.symbols")
wasm = (destination / "core.wasm").read_bytes()
record = {"variant": variant, "bytes": len(wasm),
          "wasmSha256": hashlib.sha256(wasm).hexdigest(),
          "jsSha256": hashlib.sha256((destination / "core.js").read_bytes()).hexdigest(),
          "buildDirectory": f"build/{variant}", "fullCMakeBuild": True}
path = OUT / "artifacts.json"
records = json.loads(path.read_text()) if path.exists() else []
records = [item for item in records if item["variant"] != record["variant"]]
path.write_text(json.dumps(records + [record], indent=2) + "\n")
print(json.dumps(record))
