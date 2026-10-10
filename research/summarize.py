import json
import pathlib
import statistics

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "build/research"
summary = {}
for directory in ["browser", "simd-browser", "confirmation", "cpu-browser", "cpu-confirmation"]:
    path = OUT / directory / "report.json"
    if not path.exists():
        continue
    trials = json.loads(path.read_text())["trials"]
    grouped = {}
    for variant in sorted({t["variant"] for t in trials}):
        runs = [t for t in trials if t["variant"] == variant]
        means = [statistics.mean(statistics.mean(p["times"]) for p in t["players"]) for t in runs]
        values = sorted(v for t in runs for p in t["players"] for v in p["times"])
        grouped[variant] = {"trialPairMeansMs": means, "meanMs": statistics.mean(values),
                            "p95Ms": values[int(len(values)*.95)],
                            "coreOnlyFps": 1000/statistics.mean(values),
                            "allPassed": all(t["ok"] for t in runs),
                            "checkpoints": sum(len(p["checks"]) for t in runs for p in t["players"])}
    summary[directory] = grouped
    path.with_name("summary.json").write_text(json.dumps(grouped, indent=2)+"\n")
(OUT / "summary.json").write_text(json.dumps(summary,indent=2)+"\n")
print(json.dumps(summary,indent=2))
