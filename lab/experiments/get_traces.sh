#!/usr/bin/env bash
# Downloads the three public CDN traces used in the evaluation and converts them to the
# cache-lab binary format. Needs ~3 GB of disk and streams only the parts used.
#   W18  : Wikipedia CDN 2018 (released with Song et al., "Learning Relaxed Belady", NSDI'20),
#          first 50M requests of https://lrb.cs.princeton.edu/wiki2018.tr.tar.gz (16.8 GB)
#   W19u : Wikimedia caching dataset 2019, upload (media) cluster, day 01, first 40M requests
#   W19t : Wikimedia caching dataset 2019, text (HTML) cluster, day 01, all requests
# Usage: lab/experiments/get_traces.sh <out-dir>
set -euo pipefail
OUT=${1:?out dir}
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/cachelab"
mkdir -p "$OUT"
cd "$OUT"
WM=https://analytics.wikimedia.org/published/datasets/caching/2019

# "timestamp object size" text, then binary. head closes the pipe early on purpose.
(curl -sS https://lrb.cs.princeton.edu/wiki2018.tr.tar.gz | tar -xzO 2>/dev/null | head -n 50000000 \
  | awk '{print $1" "$2" "$3}' > w18.txt) || true
(curl -sS "$WM/upload/cache-u-01.gz" | gunzip 2>/dev/null | head -n 40000001 \
  | awk -F'\t' 'NR>1{print $1" "$2" "$4}' > w19u.txt) || true
curl -sS "$WM/text/cache-t-01.gz" | gunzip | awk -F'\t' 'NR>1{print $1" "$2" "$3}' > w19t.txt

for t in w18 w19u w19t; do "$BIN" convert "$t.txt" "$t.bin"; done
for t in w18 w19u w19t; do "$BIN" stats "$t.bin"; done
