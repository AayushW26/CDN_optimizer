import { useState } from 'react';
import { CompareFile, METRICS, fmt, metricByKey, scaleOf, useJson } from '../data';
import { ChartCard, Loading, RankedBars, Seg, Select, useMode } from './ui';
import { CompareTable } from './Overview';

const WHAT: Record<string, string> = {
  'dp-reactive': 'Exact knapsack on last epoch\'s counts. The starting point.',
  'dp-predictive': 'Exact knapsack on a Holt trend forecast ("predictive caching").',
  'pack-noforecast': 'PACK without the Hedge forecaster (uses last-epoch counts).',
  'pack-nochurn': 'PACK without the admission (churn) penalty.',
  'pack-nodrift': 'PACK without the mid-epoch burst detector.',
  'pack-window': 'PACK plus a 10% LRU admission window (W-TinyLFU style). We tried it and it hurt.',
  'pack-greedy': 'PACK with the density-greedy solver instead of exact B&B/DP.',
  'pack-fptas': 'PACK with the (1−ε) FPTAS solver, ε = 0.05.',
  pack: 'Full PACK-DP: Hedge forecaster + churn penalty + burst re-planning + exact solver.',
};

export function Ablation() {
  const { data, error } = useJson<CompareFile>('ablation.json');
  const [pattern, setPattern] = useState('mixed');
  const [metricKey, setMetricKey] = useState('benefitRatio');
  const mode = useMode();
  if (!data) return <Loading error={error} />;
  const group = data.patterns.find((p) => p.pattern === pattern) ?? data.patterns[0];
  const metric = metricByKey(metricKey);
  const full = group.algorithms.find((a) => a.key === 'pack');
  // Diverging bars from a zero baseline: each variant minus full PACK-DP on the same traces.
  // One metric -> one neutral hue; better/worse is carried by the side of zero and the signed label.
  const neutral = mode === 'dark' ? '#77756f' : '#9a9890';
  const rows = group.algorithms
    .filter((a) => a.key !== 'pack')
    .map((a) => {
      const v = ((a.mean[metricKey] as number) - (full?.mean[metricKey] as number)) * scaleOf(metric);
      return { id: a.key, name: a.name, color: neutral, value: Math.abs(v) < 0.005 ? 0 : v };
    })
    .sort((a, b) => (metric.higherBetter ? b.value - a.value : a.value - b.value));
  const unit = metric.pct || metric.unit === '%' ? ' pts' : ` ${metric.unit}`;
  const f = (v: number) => `${v > 0 ? '+' : v < 0 ? '−' : '±'}${Math.abs(v).toFixed(2)}${unit}`;
  const raw = Math.max(0.5, ...rows.map((r) => Math.abs(r.value))) * 1.1;
  const lim = [0.5, 1, 2, 3, 4, 5, 6, 8, 10, 15, 20, 30, 50, 100].find((n) => n >= raw) ?? Math.ceil(raw);
  return (
    <>
      <div className="page-intro">
        <h2>Ablation: what each PACK-DP component buys</h2>
        <p>
          Each variant removes (or swaps) one component of PACK-DP and is replayed on identical traces ({data.seeds} seeds).
          Parameters were tuned on held-out seeds (101–102), not on these evaluation seeds.
        </p>
      </div>
      <div className="filters">
        <Seg label="Traffic" options={data.patterns.map((p) => ({ value: p.pattern, label: p.pattern }))} value={pattern} onChange={setPattern} />
        <Select label="Metric" options={METRICS.filter((m) => m.key !== 'burstHitRatio').map((m) => ({ value: m.key, label: m.label }))} value={metricKey} onChange={setMetricKey} />
      </div>
      <div className="grid grid-2">
        <ChartCard className="span-2" title={`${metric.label}: each variant minus full PACK-DP (${fmt(full?.mean[metricKey], metric, 2)})`}
          sub={`Left of zero = removing or swapping that component made PACK-DP worse (${metric.higherBetter ? 'higher' : 'lower'} is better). Mean over ${data.seeds} seeds on identical traces.`}
          table={() => <CompareTable algos={group.algorithms} />}>
          <RankedBars rows={rows} fmt={f} domain={[-lim, lim]} ticks={[-lim, -lim / 2, 0, lim / 2, lim]} />
        </ChartCard>
        <ChartCard className="span-2" title="Variant guide" sub="Δ is the variant's benefit minus full PACK-DP on this traffic pattern.">
          <div className="table-wrap"><table>
            <thead><tr><th>Variant</th><th style={{ textAlign: 'left' }}>What changes</th><th>Benefit</th><th>Δ vs PACK</th></tr></thead>
            <tbody>
              {group.algorithms.map((a) => {
                const d0 = ((a.mean.benefitRatio as number) - (full?.mean.benefitRatio as number)) * 100;
                const d = Math.abs(d0) < 0.005 ? 0 : d0;
                return (
                  <tr key={a.key} className={a.key === 'pack' ? 'hl' : ''}>
                    <td>{a.name}</td>
                    <td style={{ textAlign: 'left', whiteSpace: 'normal' }}>{WHAT[a.key] ?? ''}</td>
                    <td>{fmt(a.mean.benefitRatio, 'benefitRatio', 2)}</td>
                    <td className={a.key === 'pack' ? '' : d < 0 ? 'down' : 'up'}>{a.key === 'pack' ? '–' : `${d >= 0 ? '+' : ''}${d.toFixed(2)}`}</td>
                  </tr>
                );
              })}
            </tbody>
          </table></div>
        </ChartCard>
      </div>
    </>
  );
}
