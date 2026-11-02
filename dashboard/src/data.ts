// Data types (mirroring the C++ JSON exporters), loaders, palette and metric catalog.
import { useEffect, useState } from 'react';

export type Fields = Record<string, number | null>;

export interface AggAlgo { key: string; name: string; oracle: boolean; mean: Fields; std: Fields }
export interface CompareFile {
  experiment: string;
  seeds: number;
  config: Record<string, number | string>;
  patterns: { pattern: string; algorithms: AggAlgo[]; pairedVsPack?: { key: string; meanDelta: number; stdDelta: number; packWins: number; runs: number }[] }[];
}
export interface SweepSeries { key: string; name: string; oracle: boolean; mean: Record<string, (number | null)[]>; std: Record<string, (number | null)[]> }
export interface SweepFile {
  experiment: string;
  xLabel: string;
  seeds: number;
  x: number[];
  patterns: { pattern: string; series: SweepSeries[] }[];
}
export interface PlanEvent {
  epoch: number; at: number; reason: string; solver: string; candidates: number; solveMs: number;
  planValue: number; expectedHitRatio: number; plannedMB: number; admitted: number[]; evicted: number[];
}
export interface RunAlgo {
  key: string; name: string; oracle: boolean; summary: Fields;
  epochs?: Record<string, number[]>;
  finalContents?: [number, number, number, number][];
  finalContentsTotal?: number;
  events?: PlanEvent[];
}
export interface RunFile {
  config: Record<string, number | string>;
  capacityKB: number;
  wallMs: number;
  catalog: { objects: number; totalGB: number; static: number; video: number; large: number };
  markers: { epoch: number; start: number; type: string; label: string }[];
  algorithms: RunAlgo[];
}
export interface SolverAlgo { name: string; ms: (number | null)[]; memKB: (number | null)[]; gapPct: (number | null)[]; nodes: (number | null)[]; logLogSlope: number | null }
export interface SolverBlock { variable: string; x: number[]; algorithms: SolverAlgo[] }
export interface SolversFile {
  scalingN: SolverBlock; scalingC: SolverBlock; hardCorrelated: SolverBlock; hugeCapacity: SolverBlock;
  fptasEps: { eps: number[]; ms: number[]; gapPct: number[]; memKB: number[]; gapPctHard: number[] };
}
export interface OptGapFile {
  x: number[]; instances: number;
  methods: { name: string; meanGapPct: number[]; maxGapPct: number[] }[];
  boundPct: number[]; lpSlackPct: number[]; lpSlackMaxPct: number[]; bnbGapPct: number[];
  items: number[]; provenFraction: number[]; bnbMs: number[];
}
export interface AbrFile {
  seeds: number; videos: number; transcodeKeep: number; mckpMaxMs: number; mckpMemKB: number;
  ladder: { name: string; kbps: number; share: number }[];
  x: number[]; xLabel: string;
  policies: ({ name: string } & Record<string, number[]>)[];
}

// ---------------------------------------------------------------- loading --
const cache = new Map<string, Promise<unknown>>();
export function loadJson<T>(name: string): Promise<T> {
  if (!cache.has(name)) {
    cache.set(
      name,
      fetch(`data/${name}`).then((r) => {
        if (!r.ok) throw new Error(`${name}: HTTP ${r.status}`);
        return r.json();
      }),
    );
  }
  return cache.get(name) as Promise<T>;
}

export function useJson<T>(name: string | null): { data: T | null; error: string | null } {
  const [state, setState] = useState<{ data: T | null; error: string | null }>({ data: null, error: null });
  useEffect(() => {
    if (!name) return;
    let alive = true;
    setState((s) => ({ data: s.data, error: null }));
    loadJson<T>(name)
      .then((d) => alive && setState({ data: d, error: null }))
      .catch((e: Error) => alive && setState({ data: null, error: e.message }));
    return () => {
      alive = false;
    };
  }, [name]);
  return state;
}

// ---------------------------------------------------------------- theming --
export type Mode = 'light' | 'dark';

export function useResolvedMode(pref: 'auto' | Mode): Mode {
  const mq = typeof window !== 'undefined' ? window.matchMedia('(prefers-color-scheme: dark)') : null;
  const [sys, setSys] = useState<Mode>(mq?.matches ? 'dark' : 'light');
  useEffect(() => {
    if (!mq) return;
    const h = (e: MediaQueryListEvent) => setSys(e.matches ? 'dark' : 'light');
    mq.addEventListener('change', h);
    return () => mq.removeEventListener('change', h);
  }, [mq]);
  return pref === 'auto' ? sys : pref;
}

/** Categorical palette (validated: adjacent CVD dE >= 9.1 light / 8.4 dark). */
const SLOTS: Record<Mode, string[]> = {
  light: ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#6250d6', '#e34948'],
  dark: ['#3987e5', '#d95926', '#199e70', '#c98500', '#d55181', '#008300', '#9085e9', '#e66767'],
};
export const CHROME: Record<Mode, { grid: string; axis: string; muted: string; ink2: string; surface: string; oracleA: string; oracleB: string }> = {
  light: { grid: '#e1e0d9', axis: '#c3c2b7', muted: '#6f6e69', ink2: '#52514e', surface: '#fcfcfb', oracleA: '#52514e', oracleB: '#a3a19a' },
  dark: { grid: '#2c2c2a', axis: '#383835', muted: '#9a9890', ink2: '#c3c2b7', surface: '#1a1a19', oracleA: '#c3c2b7', oracleB: '#77756f' },
};

/** Colour follows the entity: each algorithm owns a fixed slot everywhere in the app. */
const SLOT_OF: Record<string, number> = {
  pack: 0, lru: 1, lfu: 2, gdsf: 3, 'greedy-epoch': 4, 'dp-reactive': 5, 'dp-predictive': 6, 'dp-ewma': 7, 'dp-units': 7,
};

export function algoColor(key: string, mode: Mode): string {
  if (key === 'dp-oracle') return CHROME[mode].oracleA;
  if (key === 'belady') return CHROME[mode].oracleB;
  const s = SLOT_OF[key];
  return SLOTS[mode][s ?? 7];
}
export function slotColor(i: number, mode: Mode): string {
  return SLOTS[mode][i % 8];
}
export const isOracleKey = (k: string) => k === 'dp-oracle' || k === 'belady';

// ---------------------------------------------------------------- metrics --
export interface Metric { key: string; label: string; short: string; unit: string; pct: boolean; higherBetter: boolean; help: string }
export const METRICS: Metric[] = [
  { key: 'benefitRatio', label: 'Benefit captured', short: 'Benefit', unit: '%', pct: true, higherBetter: true,
    help: 'Share of the total caching benefit B = f·(α·cost + β·latency) served from the edge. This is the knapsack objective.' },
  { key: 'hitRatio', label: 'Request hit ratio', short: 'Hit ratio', unit: '%', pct: true, higherBetter: true, help: 'Requests served from cache.' },
  { key: 'byteHitRatio', label: 'Byte hit ratio', short: 'Byte hit', unit: '%', pct: true, higherBetter: true, help: 'Bytes served from cache.' },
  { key: 'costSavedPct', label: 'Origin $ saved', short: '$ saved', unit: '%', pct: false, higherBetter: true,
    help: 'Origin egress dollars avoided, net of prefetch traffic, vs. no cache.' },
  { key: 'avgLatencyMs', label: 'Mean latency', short: 'Latency', unit: 'ms', pct: false, higherBetter: false, help: 'Hit: edge RTT. Miss: edge RTT + origin TTFB.' },
  { key: 'originOffloadPct', label: 'Origin offload', short: 'Offload', unit: '%', pct: false, higherBetter: true,
    help: 'Bytes not pulled from origin, counting proactive prefetches as origin traffic.' },
  { key: 'burstHitRatio', label: 'Hit ratio at flash-crowd onset', short: 'Burst hit', unit: '%', pct: true, higherBetter: true,
    help: 'Hit ratio during the epoch a flash crowd starts and the one after.' },
  { key: 'planMs', label: 'Planning time (total)', short: 'Plan ms', unit: 'ms', pct: false, higherBetter: false, help: 'Total time spent in knapsack solvers.' },
  { key: 'nsPerRequest', label: 'Per-request cost', short: 'ns/req', unit: 'ns', pct: false, higherBetter: false, help: 'Average time of one access() call.' },
];
export const metricByKey = (k: string) => METRICS.find((m) => m.key === k) ?? METRICS[0];

export function fmt(v: number | null | undefined, m: Metric | string, digits?: number): string {
  if (v === null || v === undefined || !Number.isFinite(v)) return '–';
  const metric = typeof m === 'string' ? metricByKey(m) : m;
  if (metric.pct) return `${(v * 100).toFixed(digits ?? 1)}%`;
  if (metric.unit === '%') return `${v.toFixed(digits ?? 1)}%`;
  if (metric.unit === 'ms' && v >= 1000) return `${(v / 1000).toFixed(digits ?? 1)} s`;
  return `${v.toFixed(digits ?? (Math.abs(v) < 10 ? 2 : 1))}${metric.unit ? ' ' + metric.unit : ''}`;
}
export const scaleOf = (m: Metric) => (m.pct ? 100 : 1);

export const PATTERN_INFO: Record<string, string> = {
  uniform: 'Every object equally likely - no locality to exploit (worst case for any cache).',
  zipf: 'Stationary Zipf(0.9) popularity - the classic web/video skew.',
  flash: 'Zipf background plus sudden flash crowds on previously-cold objects (viral content).',
  drift: 'Popularity churn of the head plus content lifecycles (rise, peak, fade).',
  mixed: 'Realistic CDN: drift + lifecycles + flash crowds + crawler scans (one-hit wonders).',
};
export const PATTERNS = ['uniform', 'zipf', 'flash', 'drift', 'mixed'];
