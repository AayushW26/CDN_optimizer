#!/usr/bin/env bash
# Held-out evaluation. SHADE was frozen on the development slice (W18 requests 0-10M)
# before any of these runs. Usage: lab/experiments/run_heldout.sh <trace-dir> <out-dir>
set -euo pipefail
TRACES=${1:?trace dir}
OUT=${2:?out dir}
BIN="$(dirname "$0")/../build/cachelab"
mkdir -p "$OUT"
SIZES=0.001,0.003,0.01,0.03,0.1
MAIN="LRU,FIFO,SIEVE,S3-FIFO,GDSF,W-TinyLFU,AdaptSize,LHD,SHADE,SHADE-B,Belady-Size*"
ABL="SHADE,SHADE-noAdmit,SHADE-noGap,SHADE-noCount,SHADE-noFeatures,SHADE:index=0:h=4,SHADE:km=0,SHADE:pool=0,SHADE:shrink=0,SHADE:exchange=0,SHADE:slack=1"

"$BIN" run "$TRACES/w18.bin" --offset 10000000 --limit 40000000 --sizes $SIZES --policies "$MAIN" --out "$OUT/w18test_main.json"
"$BIN" run "$TRACES/w19u.bin" --sizes $SIZES --policies "$MAIN" --out "$OUT/w19upload_main.json"
"$BIN" run "$TRACES/w19t.bin" --sizes $SIZES --policies "$MAIN" --out "$OUT/w19text_main.json"
# development slice, for reference (this is where the method was designed)
"$BIN" run "$TRACES/w18.bin" --limit 10000000 --sizes $SIZES --policies "$MAIN" --out "$OUT/w18dev_main.json"
# ablations at three sizes on every held-out trace
"$BIN" run "$TRACES/w18.bin" --offset 10000000 --limit 40000000 --sizes 0.001,0.01,0.1 --policies "$ABL" --out "$OUT/w18test_ablation.json"
"$BIN" run "$TRACES/w19u.bin" --sizes 0.001,0.01,0.1 --policies "$ABL" --out "$OUT/w19upload_ablation.json"
"$BIN" run "$TRACES/w19t.bin" --sizes 0.001,0.01,0.1 --policies "$ABL" --out "$OUT/w19text_ablation.json"
