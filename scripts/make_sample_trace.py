#!/usr/bin/env python3
"""Generate a small synthetic access log in the CSV format `cdnopt run --trace` accepts.

Format: timestamp,object_id,size_kb   (header required; timestamp column is optional)

Real logs (CDN / proxy access logs, the public Wikipedia CDN traces, ...) can be
converted to the same columns and replayed with:
    ./core/build/cdnopt run --trace my_log.csv --algos all
"""
import argparse
import math
import random

ap = argparse.ArgumentParser()
ap.add_argument("--objects", type=int, default=2000)
ap.add_argument("--requests", type=int, default=200000)
ap.add_argument("--zipf", type=float, default=0.9)
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--out", default="experiments/traces/sample_trace.csv")
a = ap.parse_args()

rng = random.Random(a.seed)
sizes = []
for _ in range(a.objects):
    u = rng.random()
    if u < 0.6:
        sizes.append(max(2, int(rng.lognormvariate(math.log(60), 1.0))))
    elif u < 0.92:
        sizes.append(max(256, int(rng.lognormvariate(math.log(2500), 0.6))))
    else:
        sizes.append(max(8192, int(rng.lognormvariate(math.log(60000), 0.8))))
weights = [1 / (r + 1) ** a.zipf for r in range(a.objects)]
ids = [f"/obj/{rng.getrandbits(40):010x}" for _ in range(a.objects)]
t = 1_700_000_000.0
with open(a.out, "w") as f:
    f.write("timestamp,object_id,size_kb\n")
    for i in rng.choices(range(a.objects), weights=weights, k=a.requests):
        t += rng.expovariate(2000.0)
        f.write(f"{t:.3f},{ids[i]},{sizes[i]}\n")
print(f"wrote {a.out}: {a.requests} requests over {a.objects} objects")
