// Zero-dependency HTTP bridge between the React dashboard and the C++ engine.
//
//   GET  /api/health          -> { ok, binary }
//   POST /api/simulate {...}   -> JSON produced by `cdnopt run --out - --detail`
//   GET  /*                    -> static files from ../dashboard/dist
//
// Inputs are validated against an allow-list and passed to execFile as an
// argument vector (never through a shell). At most MAX_JOBS simulations run at once.
import { execFile } from 'node:child_process';
import { createReadStream, existsSync, statSync } from 'node:fs';
import { createServer } from 'node:http';
import { dirname, extname, join, normalize, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const PORT = Number(process.env.PORT ?? 8787);
const HOST = process.env.HOST ?? '127.0.0.1';
const BIN = process.env.CDNOPT_BIN ?? resolve(here, '..', 'core', 'build', 'cdnopt');
const STATIC = resolve(here, '..', 'dashboard', 'dist');
const MAX_JOBS = Number(process.env.MAX_JOBS ?? 2);
const MAX_REQUESTS = 2_000_000;

const PATTERNS = new Set(['uniform', 'zipf', 'flash', 'drift', 'mixed']);
const ALGOS = new Set(['lru', 'lfu', 'gdsf', 'belady', 'greedy-epoch', 'dp-reactive', 'dp-units', 'dp-ewma', 'dp-predictive',
  'dp-oracle', 'pack', 'pack-noforecast', 'pack-nochurn', 'pack-nodrift', 'pack-window', 'pack-greedy', 'pack-fptas']);
const NUMERIC = {
  capacity: [0.001, 0.5, 'capacity'], alpha: [0, 10, 'alpha'], beta: [0, 10, 'beta'], objects: [100, 50000, 'objects'],
  epochs: [2, 100, 'epochs'], epochLen: [500, 100000, 'epoch-len'], seed: [0, 1e9, 'seed'],
};
const MIME = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json',
  '.svg': 'image/svg+xml', '.png': 'image/png', '.ico': 'image/x-icon' };

let running = 0;

function send(res, code, body, type = 'application/json') {
  res.writeHead(code, { 'Content-Type': type, 'Cache-Control': 'no-store' });
  res.end(typeof body === 'string' ? body : JSON.stringify(body));
}

function buildArgs(body) {
  const args = ['run', '--out', '-', '--detail'];
  const pattern = String(body.pattern ?? 'zipf');
  if (!PATTERNS.has(pattern)) throw new Error(`unknown pattern '${pattern}'`);
  args.push('--pattern', pattern);
  for (const [key, [lo, hi, flag]] of Object.entries(NUMERIC)) {
    if (body[key] === undefined) continue;
    const v = Number(body[key]);
    if (!Number.isFinite(v) || v < lo || v > hi) throw new Error(`${key} must be in [${lo}, ${hi}]`);
    args.push(`--${flag}`, String(v));
  }
  const epochs = Number(body.epochs ?? 40), len = Number(body.epochLen ?? 15000);
  if (epochs * len > MAX_REQUESTS) throw new Error(`epochs x epochLen must be <= ${MAX_REQUESTS.toLocaleString()}`);
  const algos = Array.isArray(body.algos) ? body.algos.map(String) : [];
  if (algos.length === 0 || algos.length > 17 || algos.some((a) => !ALGOS.has(a))) throw new Error('invalid algos list');
  args.push('--algos', algos.join(','));
  return args;
}

function readBody(req, limit = 16 * 1024) {
  return new Promise((resolveBody, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', (c) => {
      size += c.length;
      if (size > limit) {
        reject(new Error('body too large'));
        req.destroy();
      } else chunks.push(c);
    });
    req.on('end', () => resolveBody(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

async function simulate(req, res) {
  let args;
  try {
    args = buildArgs(JSON.parse((await readBody(req)) || '{}'));
  } catch (e) {
    return send(res, 400, { error: e.message });
  }
  if (running >= MAX_JOBS) return send(res, 429, { error: 'simulator busy, try again in a moment' });
  running++;
  execFile(BIN, args, { timeout: 180_000, maxBuffer: 128 * 1024 * 1024 }, (err, stdout, stderr) => {
    running--;
    if (err) return send(res, 500, { error: (stderr || err.message).toString().slice(0, 500) });
    send(res, 200, stdout);
  });
}

function serveStatic(req, res) {
  if (!existsSync(STATIC)) return send(res, 404, 'Dashboard not built. Run `npm run build` in dashboard/.', 'text/plain');
  const urlPath = decodeURIComponent(new URL(req.url, 'http://x').pathname);
  let file = normalize(join(STATIC, urlPath));
  if (!file.startsWith(STATIC + sep) && file !== STATIC) return send(res, 403, 'forbidden', 'text/plain');
  if (!existsSync(file) || statSync(file).isDirectory()) file = join(STATIC, 'index.html');
  res.writeHead(200, { 'Content-Type': MIME[extname(file)] ?? 'application/octet-stream' });
  createReadStream(file).pipe(res);
}

createServer((req, res) => {
  const path = new URL(req.url, 'http://x').pathname;
  if (path === '/api/health') return send(res, existsSync(BIN) ? 200 : 503, { ok: existsSync(BIN), binary: BIN, running });
  if (path === '/api/simulate' && req.method === 'POST') return simulate(req, res);
  if (path.startsWith('/api/')) return send(res, 404, { error: 'not found' });
  if (req.method !== 'GET' && req.method !== 'HEAD') return send(res, 405, { error: 'method not allowed' });
  serveStatic(req, res);
}).listen(PORT, HOST, () => {
  console.log(`CDN Cache Optimizer on http://${HOST}:${PORT}  (engine: ${BIN})`);
});
