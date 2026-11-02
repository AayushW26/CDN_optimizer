import { useState } from 'react';
import { AggAlgo, CompareFile, METRICS, PATTERN_INFO, PATTERNS, algoColor, fmt, metricByKey, scaleOf, useJson } from '../data';
import { ChartCard, Loading, RankedBars, Seg, Select, Stat, useMode } from './ui';

function bestDeployable(algos: AggAlgo[], metric: string, higherBetter: boolean, exclude: string[] = []) {
  const pool = algos.filter((a) => !a.oracle && !exclude.includes(a.key) && a.mean[metric] != null);
  pool.sort((a, b) => ((b.mean[metric] as number) - (a.mean[metric] as number)) * (higherBetter ? 1 : -1));
  return pool[0];
}

export function Overview() {
  const { data, error } = useJson<CompareFile>('compare.json');
  const [pattern, setPattern] = useState('mixed');
  const [metricKey, setMetricKey] = useState('benefitRatio');
  const mode = useMode();
  if (!data) return <Loading error={error} />;
  const group = data.patterns.find((p) => p.pattern === pattern) ?? data.patterns[0];
  const metric = metricByKey(metricKey);
  const pack = group.algorithms.find((a) => a.key === 'pack');
  const rival = bestDeployable(group.algorithms, 'benefitRatio', true, ['pack']);
  const oracle = group.algorithms.find((a) => a.key === 'dp-oracle');
  const lru = group.algorithms.find((a) => a.key === 'lru');

  const rows = group.algorithms
    .filter((a) => a.mean[metricKey] != null && Number.isFinite(a.mean[metricKey] as number))
    .map((a) => ({
      id: a.key, name: a.name, color: algoColor(a.key, mode), dashed: a.oracle,
      value: (a.mean[metricKey] as number) * scaleOf(metric), std: ((a.std[metricKey] as number) ?? 0) * scaleOf(metric),
    }))
    .sort((a, b) => (metric.higherBetter ? b.value - a.value : a.value - b.value));
  const f = (v: number) => (metric.pct ? `${v.toFixed(1)}%` : fmt(v, metric));

  const gap = (a?: AggAlgo, b?: AggAlgo, k = 'benefitRatio') =>
    a && b ? ((a.mean[k] as number) - (b.mean[k] as number)) * 100 : null;
  const closed = pack && rival && oracle
    ? (((pack.mean.benefitRatio as number) - (rival.mean.benefitRatio as number)) /
        Math.max(1e-9, (oracle.mean.benefitRatio as number) - (rival.mean.benefitRatio as number))) * 100
    : null;

  return (
    <>
      <div className="page-intro">
        <h2>Which objects should an edge cache keep?</h2>
        <p>
          Each epoch, the cache picks the subset of objects that maximises total caching benefit
          <code>B<sub>i</sub> = f<sub>i</sub>·(α·c<sub>i</sub> + β·l<sub>i</sub>)</code> subject to <code>Σ s<sub>i</sub> ≤ C</code>.
          That is a 0/1 knapsack. Below, every algorithm replays the <em>same</em> request stream ({data.seeds} seeds, mean ± s.d.).
          <strong> PACK-DP</strong> (Predictive, Adaptive, Churn-aware Knapsack) is this project's planner. Dashed entries are
          oracles that see the future and serve only as reference bounds.
        </p>
      </div>
      <div className="filters">
        <Seg label="Traffic" options={PATTERNS.map((p) => ({ value: p, label: p }))} value={pattern} onChange={setPattern} />
        <Select label="Metric" options={METRICS.map((m) => ({ value: m.key, label: m.label }))} value={metricKey} onChange={setMetricKey} />
      </div>
      <div className="grid grid-4" style={{ marginBottom: 16 }}>
        <Stat label="PACK-DP benefit captured" value={fmt(pack?.mean.benefitRatio, 'benefitRatio')}
          delta={rival ? <>{gap(pack, rival)! >= 0 ? '+' : ''}{gap(pack, rival)!.toFixed(2)} pts vs best baseline ({rival.name})</> : undefined}
          deltaGood={gap(pack, rival) !== null ? gap(pack, rival)! >= 0 : null} />
        <Stat label="Gap to hindsight optimum closed" value={closed !== null ? `${closed.toFixed(0)}%` : '–'}
          delta={oracle ? <>DP-Oracle* captures {fmt(oracle.mean.benefitRatio, 'benefitRatio')}</> : undefined} deltaGood={null} />
        <Stat label="PACK-DP origin $ saved" value={fmt(pack?.mean.costSavedPct, 'costSavedPct')}
          delta={lru ? <>{((pack!.mean.costSavedPct as number) - (lru.mean.costSavedPct as number)).toFixed(1)} pts vs LRU</> : undefined}
          deltaGood={lru ? (pack!.mean.costSavedPct as number) >= (lru.mean.costSavedPct as number) : null} />
        <Stat label="PACK-DP mean latency" value={fmt(pack?.mean.avgLatencyMs, 'avgLatencyMs', 1)}
          delta={lru ? <>{((lru.mean.avgLatencyMs as number) - (pack!.mean.avgLatencyMs as number)).toFixed(1)} ms faster than LRU</> : undefined}
          deltaGood={lru ? (pack!.mean.avgLatencyMs as number) <= (lru.mean.avgLatencyMs as number) : null} />
      </div>
      <div className="grid grid-2">
        <ChartCard
          title={`${metric.label}: ${pattern} traffic`}
          sub={`${metric.help} ${metric.higherBetter ? 'Higher' : 'Lower'} is better. Labels show mean ± 1 s.d. over ${data.seeds} seeds.`}
          className="span-2"
          table={() => <CompareTable algos={group.algorithms} />}
        >
          <RankedBars rows={rows} fmt={f} />
        </ChartCard>
        {group.pairedVsPack && (
          <ChartCard
            className="span-2"
            title="Paired comparison: PACK-DP minus each algorithm"
            sub="Same trace, same catalog, per seed. Seeds differ in catalog and traffic, so the paired difference is the fair statistic. The across-seed spread above mostly reflects catalog variation."
          >
            <div className="table-wrap"><table>
              <thead><tr><th>Algorithm</th><th>Benefit Δ (pts)</th><th>± s.d.</th><th>PACK-DP wins</th></tr></thead>
              <tbody>
                {group.pairedVsPack.filter((p) => p.key !== 'pack').map((p) => {
                  const a = group.algorithms.find((x) => x.key === p.key)!;
                  return (
                    <tr key={p.key}>
                      <td>{a.name}{a.oracle && <span className="badge">oracle</span>}</td>
                      <td className={p.meanDelta >= 0 ? 'up' : 'down'}>{p.meanDelta >= 0 ? '+' : '−'}{Math.abs(p.meanDelta * 100).toFixed(2)}</td>
                      <td>{(p.stdDelta * 100).toFixed(2)}</td>
                      <td>{p.packWins} / {p.runs}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table></div>
          </ChartCard>
        )}
        <ChartCard title="Traffic pattern" sub={PATTERN_INFO[pattern]}>
          <ul className="kbd-list">
            <li>Catalog: {data.config.numObjects} objects (images, video segments, large files), sizes from 2 KB to 500 MB.</li>
            <li>Cache: {(Number(data.config.capacityFrac) * 100).toFixed(0)}% of catalog bytes. {data.config.epochs} epochs × {Number(data.config.epochLen).toLocaleString()} requests.</li>
            <li>4 origin tiers ($0.02–0.25/GB, 35–260 ms TTFB), so values differ per object.</li>
            <li>Prefetches by planners are <strong>charged</strong> as origin traffic (no free lunch).</li>
            <li>The first {data.config.warmup} epochs are excluded as warm-up.</li>
          </ul>
        </ChartCard>
        <ChartCard title="How to read this" sub="Three families of algorithms">
          <ul className="kbd-list">
            <li><strong>Online</strong> (LRU, LFU, GDSF) decide per request in O(1)–O(log n).</li>
            <li><strong>Epoch planners</strong> solve a knapsack each epoch on <em>forecast</em> frequencies: Greedy, DP-Reactive (last epoch), DP-Predictive (Holt), PACK-DP.</li>
            <li><strong>Oracles*</strong>: DP-Oracle solves the knapsack on the epoch's <em>true</em> counts; Belady evicts by furthest next use. Neither is deployable.</li>
            <li>The headline finding: the exact-vs-greedy optimisation gap is tiny (see <em>DAA lab</em>), while the forecast gap is large, so PACK spends its effort on prediction and adaptation.</li>
          </ul>
        </ChartCard>
      </div>
    </>
  );
}

export function CompareTable({ algos }: { algos: AggAlgo[] }) {
  const cols = ['benefitRatio', 'hitRatio', 'byteHitRatio', 'costSavedPct', 'avgLatencyMs', 'originOffloadPct', 'planMs', 'nsPerRequest'];
  const best: Record<string, number> = {};
  for (const k of cols) {
    const m = metricByKey(k);
    const vals = algos.filter((a) => !a.oracle).map((a) => a.mean[k] as number).filter(Number.isFinite);
    best[k] = m.higherBetter ? Math.max(...vals) : Math.min(...vals);
  }
  return (
    <table>
      <thead>
        <tr>
          <th>Algorithm</th>
          {cols.map((k) => <th key={k}>{metricByKey(k).short}</th>)}
        </tr>
      </thead>
      <tbody>
        {algos.map((a) => (
          <tr key={a.key} className={a.key === 'pack' ? 'hl' : ''}>
            <td>{a.name}{a.oracle && <span className="badge">oracle</span>}</td>
            {cols.map((k) => (
              <td key={k}>
                <span className={!a.oracle && a.mean[k] === best[k] ? 'best' : ''}>{fmt(a.mean[k], k, k === 'planMs' || k === 'nsPerRequest' ? 0 : undefined)}</span>
              </td>
            ))}
          </tr>
        ))}
      </tbody>
    </table>
  );
}
