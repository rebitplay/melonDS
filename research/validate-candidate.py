"""Run recovery/output checks sequentially; never alongside timing trials."""
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CLIENT = ROOT.parent / "rebit-client"
OUT = ROOT / "build/research"
variant = sys.argv[1] if len(sys.argv) > 1 else "simd-full"
RESULTS = OUT / f"{variant}-validation" if len(sys.argv) > 1 else OUT
RESULTS.mkdir(parents=True, exist_ok=True)
reference_variant = sys.argv[2] if len(sys.argv) > 2 else "pinned"
baseline = CLIENT / "public/cores/melonds_dual_cooperative" if reference_variant == "pinned" else OUT / reference_variant / "core"
candidate = OUT / variant / "core"
rom = ROOT.parent / "rebit/public/test-roms/Mario-Kart-DS-USA.nds"
checkpoint = CLIENT / "artifacts/nds-review-2026-10-03/race.bin"
proof = ROOT / "rebit/emscripten/tests/proof-cooperative-memory.cjs"
modules = RESULTS / "proof-modules"
modules.mkdir(parents=True, exist_ok=True)
# The client package is ESM, while the generated Emscripten loader is CommonJS
# in Node. Copy bytes unchanged to explicit .cjs paths for this existing proof.
shutil.copyfile(str(baseline) + ".js", modules / "pinned.cjs")
shutil.copyfile(str(candidate) + ".js", modules / "candidate.cjs")

subprocess.run(["node", "--test", "rebit/emscripten/tests/arm-dispatch.test.cjs",
                "rebit/emscripten/tests/state-difference.test.cjs"], cwd=ROOT, check=True)
subprocess.run(["node", str(proof), str(modules / "pinned.cjs"), str(baseline) + ".wasm",
                str(modules / "candidate.cjs"), str(candidate) + ".wasm", str(rom), str(checkpoint),
                str(RESULTS / "recovery-proof.json")], cwd=ROOT, check=True)
subprocess.run(["node", str(CLIENT / "scripts/proof-nds-cooperative-browsers.mjs"), str(proof),
                str(candidate) + ".js", str(candidate) + ".wasm", str(rom), str(checkpoint),
                str(RESULTS / "recovery-proof.json"), str(RESULTS / "browser-recovery-proof.json")],
               cwd=CLIENT, check=True, env={**os.environ, "NDS_PROOF_BROWSERS": "chromium,firefox"})
