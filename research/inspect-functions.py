"""Record actual final WASM function body sizes using exact artifact symbol maps."""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/research"


def leb(data, offset):
    result = shift = 0
    while True:
        value = data[offset]; offset += 1
        result |= (value & 127) << shift
        if not value & 128: return result, offset
        shift += 7


for variant in sys.argv[1:]:
    data = (OUT / variant / "core.wasm").read_bytes()
    offset = 8; imports = 0; sizes = []
    while offset < len(data):
        section = data[offset]; offset += 1
        length, offset = leb(data, offset); end = offset + length
        if section == 2:
            count, cursor = leb(data, offset)
            for _ in range(count):
                for _ in range(2):
                    size, cursor = leb(data, cursor); cursor += size
                kind = data[cursor]; cursor += 1
                assert kind == 0, "This inspector expects cooperative function-only imports"
                imports += 1; _, cursor = leb(data, cursor)
        if section == 10:
            count, cursor = leb(data, offset)
            for _ in range(count):
                size, cursor = leb(data, cursor); sizes.append(size); cursor += size
        offset = end
    names = {int(line.split(":", 1)[0]): re.sub(r"\\([0-9a-f]{2})", lambda m:chr(int(m[1],16)),line.split(":",1)[1])
             for line in (OUT / variant / "core.js.symbols").read_text().splitlines()}
    functions = [{"name": name, "index": index, "bodyBytes": sizes[index-imports]}
                 for index, name in names.items() if "DispatchARM" in name or "DispatchTHUMB" in name or name == "md_run_frame"]
    result = {"wasmBytes": len(data), "imports": imports, "functions": functions}
    (OUT / variant / "function-sizes.json").write_text(json.dumps(result, indent=2)+"\n")
    print(variant, json.dumps(result, indent=2))
