# Server: dashboard + live simulation API

Zero-dependency Node (≥ 18) bridge between the dashboard and the C++ engine.

```bash
make build && (cd dashboard && npm ci && npm run build)   # once
node server/server.mjs                                    # http://127.0.0.1:8787
```

| Endpoint | |
|---|---|
| `GET /api/health` | `{ ok, binary, running }` |
| `POST /api/simulate` | body `{ pattern, capacity, alpha, beta, objects, epochs, epochLen, seed, algos[] }` → the JSON written by `cdnopt run --out - --detail` |
| `GET /*` | static files from `dashboard/dist` |

Safety: every field is checked against an allow-list or a numeric range, the binary is started with
`execFile` (an argument vector, never a shell), requests are capped at 2·10⁶ per simulation, the body
at 16 KB, and at most `MAX_JOBS` (default 2) simulations run at once.

Environment variables: `PORT` (8787), `HOST` (127.0.0.1), `CDNOPT_BIN`, `MAX_JOBS`.
For development with hot reload, run this server and `npm run dev` in `dashboard/` (Vite proxies `/api`).
