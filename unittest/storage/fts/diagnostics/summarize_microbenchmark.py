"""Summarize retained diagnostic samples without discarding slower runs."""
import json
from pathlib import Path
import statistics

root = Path(__file__).resolve().parent
summary = {}
for path in sorted(root.glob("*-results.json")):
    result = json.loads(path.read_text(encoding="utf-8"))
    items = []
    for optimization in ("O2", "O3"):
        groups = {}
        for line in result[optimization].strip().splitlines():
            corpus, pair, before, after, checksum = line.split(",")
            groups.setdefault(corpus, []).append((float(before), float(after), checksum))
        for corpus, samples in groups.items():
            assert len(samples) == 6
            assert len({s[2] for s in samples}) == 1
            before = statistics.median(s[0] for s in samples)
            after = statistics.median(s[1] for s in samples)
            items.append({"optimization": optimization, "corpus": corpus,
                          "before_median_ms": before, "after_median_ms": after,
                          "reduction_percent": (1 - after / before) * 100,
                          "samples": len(samples), "checksum": samples[0][2]})
    summary[path.name] = items
(root / "microbenchmark-summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
for name, items in summary.items():
    print(name)
    for item in items:
        print("{optimization} {corpus}: {before_median_ms:.3f} -> {after_median_ms:.3f} ms ({reduction_percent:.2f}% reduction)".format(**item))
