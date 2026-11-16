// Types and helpers for the real-trace cache-lab results (results/lab/*.json).
import { CHROME, Mode } from './data';

export interface LabResult { policy: string; cacheFrac: number; capacityBytes: number; objectMissRatio: number; byteMissRatio: number; seconds: number }
export interface LabBound { cacheFrac: number; objectMissRatio: number; byteMissRatio: number }
export interface LabFile {
  trace: string; offset: number; requests: number; objects: number; footprintBytes: number; requestedBytes: number;
  oneHitObjectFrac: number; results: LabResult[]; bounds: LabBound[];
}

export const LAB_TRACES = [
  { id: 'w18test', label: 'Wikipedia 2018', sub: 'held-out · requests 10M–50M', heldOut: true },
  { id: 'w19upload', label: 'Wikimedia 2019 upload', sub: 'held-out · images & media', heldOut: true },
  { id: 'w19text', label: 'Wikimedia 2019 text', sub: 'held-out · HTML pages', heldOut: true },
  { id: 'w18dev', label: 'Wikipedia 2018 dev', sub: 'requests 0–10M · used to design SHADE', heldOut: false },
] as const;

export const BASELINES = ['LRU', 'FIFO', 'SIEVE', 'S3-FIFO', 'GDSF', 'W-TinyLFU', 'AdaptSize', 'LHD'];

export type LabMetric = 'objectMissRatio' | 'byteMissRatio';

/** Colour follows the policy everywhere on the lab pages. SHADE is slot 1; references are greys. */
const LAB_SLOT: Record<string, number> = {
  SHADE: 0, 'SHADE-B': 0, GDSF: 1, LHD: 2, AdaptSize: 3, 'S3-FIFO': 4, 'W-TinyLFU': 5, LRU: 6, SIEVE: 7,
};
const SLOTS: Record<Mode, string[]> = {
  light: ['#2a78d6', '#eb6834', '#1baf7a', '#eda100', '#e87ba4', '#008300', '#6250d6', '#e34948'],
  dark: ['#3987e5', '#d95926', '#199e70', '#c98500', '#d55181', '#0ca30c', '#9085e9', '#e66767'],
};
export function labColor(policy: string, mode: Mode): string {
  if (policy.endsWith('*') || policy === 'PFOO-L bound') return CHROME[mode].oracleB;
  const s = LAB_SLOT[policy];
  return s === undefined ? CHROME[mode].muted : SLOTS[mode][s];
}
export const SHADE_ACCENT = (mode: Mode) => SLOTS[mode][0];

export function byPolicy(f: LabFile, metric: LabMetric): Map<string, Map<number, number>> {
  const m = new Map<string, Map<number, number>>();
  for (const r of f.results) {
    if (!m.has(r.policy)) m.set(r.policy, new Map());
    m.get(r.policy)!.set(r.cacheFrac, r[metric]);
  }
  return m;
}
export const sizesOf = (f: LabFile) => [...new Set(f.results.map((r) => r.cacheFrac))].sort((a, b) => a - b);

export function bestBaseline(t: Map<string, Map<number, number>>, size: number): { name: string; value: number } {
  let best = { name: '', value: Infinity };
  for (const b of BASELINES) {
    const v = t.get(b)?.get(size);
    if (v !== undefined && v < best.value) best = { name: b, value: v };
  }
  return best;
}

export const pctSize = (f: number) => `${+(f * 100).toPrecision(3)}%`;
export const ratio = (v: number) => v.toFixed(3);
export const gb = (b: number) => `${(b / 1e9).toFixed(b >= 1e11 ? 0 : 1)} GB`;
export const millions = (n: number) => `${(n / 1e6).toFixed(n >= 1e7 ? 0 : 1)}M`;
