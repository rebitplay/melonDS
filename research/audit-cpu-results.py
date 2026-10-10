"""Check that the retained CPU reports describe complete runs on recorded bytes."""
import hashlib
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/research"
records = {record["variant"]: record for record in json.loads((OUT / "artifacts.json").read_text())}
assert records["cpu-final"]["wasmSha256"] == records["cpu-inline"]["wasmSha256"]
audit = {"ok": True, "runs": {}, "selectedWasmSha256": records["cpu-final"]["wasmSha256"]}
for folder, count in [("cpu-browser", 6), ("cpu-confirmation", 6), ("cpu-boot", 2)]:
    report = json.loads((OUT / folder / "report.json").read_text())
    assert report["ok"] and report["referenceVariant"] == "cpu-control"
    assert len(report["trials"]) == count
    if folder == "cpu-boot": assert report["startMode"] == "cold-boot"
    reference = report["trials"][0]["players"]
    for trial in report["trials"]:
        assert trial["ok"]
        record = records[trial["variant"]]
        assert trial["wasmSha256"] == record["wasmSha256"]
        assert trial["jsSha256"] == record["jsSha256"]
        for view, player in enumerate(trial["players"]):
            assert player["checks"] == reference[view]["checks"]
            assert len(player["times"]) == report["frames"] - report["warmupFrames"]
    audit["runs"][folder] = {"trials": count,
                            "sampledOutputRecords": sum(len(p["checks"]) for t in report["trials"] for p in t["players"])}
for variant in ["cpu-control", "cpu-inline", "cpu-outline", "cpu-final", "cpu-simd"]:
    for suffix, key in [("wasm", "wasmSha256"), ("js", "jsSha256")]:
        assert hashlib.sha256((OUT / variant / f"core.{suffix}").read_bytes()).hexdigest() == records[variant][key]
recovery = json.loads((OUT / "cpu-final-validation/recovery-proof.json").read_text())
assert recovery["ok"] and len(recovery["results"]) == 2
for view in recovery["results"]: assert view["candidate"]["samples"] == view["reference"]["samples"]
browsers = json.loads((OUT / "cpu-final-validation/browser-recovery-proof.json").read_text())
assert browsers["ok"]
assert {(r["engine"], r["view"]) for r in browsers["results"]} == {(engine, view) for engine in ["chromium", "firefox"] for view in [0, 1]}
for result in browsers["results"]:
    assert result["samples"] == recovery["results"][result["view"]]["candidate"]["samples"]
audit["browsers"] = [{"engine": r["engine"], "version": r["version"], "view": r["view"]} for r in browsers["results"]]
assert "PASS: 10240" in (OUT / "cpu-dispatch-test-final.log").read_text()
audit["compiledDispatchChecks"] = 10240
assert json.loads((OUT / "cpu-control-checkpoint-diagnostic/outputs.json").read_text())["ok"]
(OUT / "cpu-audit.json").write_text(json.dumps(audit, indent=2)+"\n")
print(json.dumps(audit, indent=2))
