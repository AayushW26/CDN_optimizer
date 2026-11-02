// Copies experiment results (../results/*.json) into public/data for the dashboard.
import { cpSync, existsSync, mkdirSync, readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const src = join(here, '..', '..', 'results');
const dst = join(here, '..', 'public', 'data');
mkdirSync(dst, { recursive: true });
if (!existsSync(src)) {
  console.warn('[sync-data] no ../results directory - run `cdnopt experiment all` first');
  process.exit(0);
}
let n = 0;
for (const f of readdirSync(src)) {
  if (f.endsWith('.json')) {
    cpSync(join(src, f), join(dst, f));
    n++;
  }
}
console.log(`[sync-data] copied ${n} result files into public/data`);
