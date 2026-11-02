#!/usr/bin/env bash
# Build, test and run the full experiment matrix, then refresh figures and dashboard data.
set -euo pipefail
cd "$(dirname "$0")/.."
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release
cmake --build core/build -j
./core/build/cdn_tests
./core/build/cdnopt experiment "${1:-all}" --out-dir results ${QUICK:+--quick}
if python3 -c "import matplotlib" 2>/dev/null; then python3 scripts/make_figures.py; fi
if [ -d dashboard/node_modules ]; then (cd dashboard && npm run sync-data); fi
echo "done: results/ updated"
