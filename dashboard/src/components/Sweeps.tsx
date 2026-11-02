import { useState } from 'react';
import { CartesianGrid, ResponsiveContainer, Scatter, ScatterChart, Tooltip, XAxis, YAxis } from 'recharts';
import { CHROME, METRICS, SweepFile, algoColor, fmt, metricByKey, scaleOf, useJson } from '../data';
import { ChartCard, Legend, LineSeriesChart, Loading, Seg, Select, useHidden, useMode } from './ui';

const SWEEPS: { file: string; label: string; logX: boolean; story: string; defaultMetric: string; xFmt: (v: number) => string }[] = [
  { file: 'capacity', label: 'Cache size', logX: true, defaultMetric: 'benefitRatio', xFmt: (v) => `${+(v * 100).toFixed(1)}%`,
    story: 'Small caches are where choosing well matters most. The gap between PACK-DP and recency/frequency heuristics is largest at 0.5-2% of the catalog.' },
  { file: 'skew', label: 'Zipf skew', logX: false, defaultMetric: 'benefitRatio', xFmt: (v) => v.toFixed(1),
    story: 'Higher skew concentrates traffic on fewer objects. Every policy improves, and the value-aware planners keep their lead.' },
  { file: 'flash', label: 'Flash-crowd intensity', logX: false, defaultMetric: 'burstHitRatio', xFmt: (v) => `${(v * 100).toFixed(0)}%`,
    story: 'Hit ratio during flash-crowd onsets. PACK-DP\'s burst detector re-plans mid-epoch; DP-Reactive and DP-Predictive must wait for the next epoch.' },
  { file: 'epoch', label: 'Epoch length', logX: true, defaultMetric: 'benefitRatio', xFmt: (v) => (v >= 1000 ? `${v / 1000}k` : String(v)),
    story: 'Short epochs mean fresh plans on noisy counts; long epochs mean stale plans. Total requests are fixed, so short epochs cost more solver time.' },
  { file: 'weights', label: 'α / β weights', logX: false, defaultMetric: 'byteHitRatio', xFmt: (v) => v.toFixed(1),
    story: 'α weights bandwidth $, β = 1−α weights latency. Moving α trades request hit ratio (latency) for byte hit ratio ($), which is a Pareto frontier.' },
  { file: 'resolution', label: 'DP resolution', logX: true, defaultMetric: 'benefitRatio', xFmt: (v) => (v >= 1024 ? `${v / 1024}k` : String(v)),
    story: 'Capacity-indexed DP must discretise sizes into C units (ceil keeps plans feasible). Rounding alone loses up to ~2% of plan value (see DAA lab), but the exact-KB slack fill recovers it, so the outcome is flat even at 128 units: O(nC) can use a small C.' },
];

export function Sweeps() {
  const [which, setWhich] = useState('capacity');
  const sw = SWEEPS.find((s) => s.file === which)!;
  const { data, error } = useJson<SweepFile>(`${which}.json`);
  const [patternSel, setPattern] = useState<string | null>(null);
  const [metricSel, setMetric] = useState<string | null>(null);
  const mode = useMode();
  const metricKey = metricSel ?? sw.defaultMetric;
  const metric = metricByKey(metricKey);
  if (!data || data.experiment !== which) return (
    <>
      <SweepPicker which={which} setWhich={(w) => { setWhich(w); setPattern(null); setMetric(null); }} />
      <Loading error={error} />
    </>
  );
  const pattern = patternSel && data.patterns.some((p) => p.pattern === patternSel) ? patternSel : data.patterns[0].pattern;
  const group = data.patterns.find((p) => p.pattern === pattern)!;
  const available = METRICS.filter((m) => group.series[0]?.mean[m.key]?.some((v) => v !== null));
  const series = group.series.map((s) => ({
    id: s.key, name: s.name, color: algoColor(s.key, mode), dashed: s.oracle,
    values: (s.mean[metricKey] ?? []).map((v) => (v === null ? null : v * scaleOf(metric))),
  }));
  const yFmt = (v: number) => (metric.pct ? `${v.toFixed(0)}%` : fmt(v, metric, v < 10 ? 1 : 0));

  return (
    <>
      <SweepPicker which={which} setWhich={(w) => { setWhich(w); setPattern(null); setMetric(null); }} />
      <div className="filters">
        {data.patterns.length > 1 && (
          <Seg label="Traffic" options={data.patterns.map((p) => ({ value: p.pattern, label: p.pattern }))} value={pattern} onChange={setPattern} />
        )}
        <Select label="Metric" options={available.map((m) => ({ value: m.key, label: m.label }))} value={metricKey} onChange={setMetric} />
      </div>
      <div className="grid grid-2">
        <ChartCard
          className={which === 'weights' ? '' : 'span-2'}
          title={`${metric.label} vs ${data.xLabel}`}
          sub={`${sw.story} Mean of ${data.seeds} seeds.`}
          table={() => (
            <table>
              <thead><tr><th>Algorithm</th>{data.x.map((x) => <th key={x}>{sw.xFmt(x)}</th>)}</tr></thead>
              <tbody>
                {series.map((s) => (
                  <tr key={s.id} className={s.id === 'pack' ? 'hl' : ''}>
                    <td>{s.name}</td>{s.values.map((v, i) => <td key={i}>{v === null ? '–' : yFmt(v)}</td>)}
                  </tr>
                ))}
              </tbody>
            </table>
          )}
        >
          <LineSeriesChart x={data.x} series={series} xLabel={data.xLabel} logX={sw.logX} xFmt={sw.xFmt} yFmt={yFmt} height={340} />
        </ChartCard>
        {which === 'weights' && <ParetoCard data={data} pattern={pattern} />}
        {which === 'capacity' && <CompetitiveCard data={data} pattern={pattern} />}
      </div>
    </>
  );
}

function SweepPicker({ which, setWhich }: { which: string; setWhich: (w: string) => void }) {
  return (
    <>
      <div className="page-intro">
        <h2>Parameter sweeps</h2>
        <p>Each sweep varies one axis of the experiment matrix and holds the others fixed. Use these charts to explain <em>why</em> an algorithm wins or loses.</p>
      </div>
      <div className="filters">
        <Seg label="Sweep" options={SWEEPS.map((s) => ({ value: s.file, label: s.label }))} value={which} onChange={setWhich} />
      </div>
    </>
  );
}

function ParetoCard({ data, pattern }: { data: SweepFile; pattern: string }) {
  const mode = useMode();
  const c = CHROME[mode];
  const { hidden, toggle } = useHidden();
  const group = data.patterns.find((p) => p.pattern === pattern)!;
  const series = group.series.map((s) => ({
    id: s.key, name: s.name, color: algoColor(s.key, mode), dashed: s.oracle,
    pts: data.x.map((a, i) => ({ a, x: (s.mean.byteHitRatio[i] ?? 0) * 100, y: (s.mean.hitRatio[i] ?? 0) * 100 })),
  }));
  return (
    <ChartCard title="Bandwidth vs latency trade-off" sub="Each point is one α setting (β = 1−α). Up = faster (more requests hit); right = cheaper (more bytes hit).">
      <Legend series={series} hidden={hidden} onToggle={toggle} />
      <ResponsiveContainer width="100%" height={300}>
        <ScatterChart margin={{ top: 8, right: 16, bottom: 18, left: 4 }}>
          <CartesianGrid stroke={c.grid} />
          <XAxis type="number" dataKey="x" domain={['auto', 'auto']} tickFormatter={(v) => `${v.toFixed(0)}%`} stroke={c.axis} tick={{ fill: c.muted, fontSize: 11.5 }}
            label={{ value: 'byte hit ratio', position: 'insideBottom', offset: -10, fill: c.muted, fontSize: 12 }} />
          <YAxis type="number" dataKey="y" domain={['auto', 'auto']} tickFormatter={(v) => `${v.toFixed(0)}%`} stroke={c.axis} tick={{ fill: c.muted, fontSize: 11.5 }} width={48} />
          <Tooltip content={({ active, payload }) => {
            if (!active || !payload?.length) return null;
            const p = payload[0].payload as { a: number; x: number; y: number };
            const s = series.find((q) => q.pts.includes(p as never));
            return (
              <div className="tip">
                <div className="tip-title">{s?.name} · α = {p.a.toFixed(1)}</div>
                <div className="tip-row"><span className="v">{p.y.toFixed(1)}%</span><span className="n">hit ratio</span></div>
                <div className="tip-row"><span className="v">{p.x.toFixed(1)}%</span><span className="n">byte hit ratio</span></div>
              </div>
            );
          }} />
          {series.filter((s) => !hidden.has(s.id)).map((s) => (
            <Scatter key={s.id} data={s.pts} fill={s.color} line={{ stroke: s.color, strokeWidth: 2, strokeDasharray: s.dashed ? '5 4' : undefined }}
              shape={(p: { cx?: number; cy?: number }) => <circle cx={p.cx} cy={p.cy} r={4} fill={s.color} stroke={c.surface} strokeWidth={2} />} isAnimationActive={false} />
          ))}
        </ScatterChart>
      </ResponsiveContainer>
    </ChartCard>
  );
}

function CompetitiveCard({ data, pattern }: { data: SweepFile; pattern: string }) {
  const mode = useMode();
  const group = data.patterns.find((p) => p.pattern === pattern)!;
  const belady = group.series.find((s) => s.key === 'belady');
  const oracle = group.series.find((s) => s.key === 'dp-oracle');
  if (!belady || !oracle) return null;
  const keys = ['lru', 'lfu', 'gdsf', 'dp-reactive', 'pack'];
  const series = keys
    .map((k) => group.series.find((s) => s.key === k))
    .filter(Boolean)
    .map((s) => ({
      id: s!.key, name: s!.name, color: algoColor(s!.key, mode),
      values: s!.mean.benefitRatio.map((v, i) => (v !== null && oracle.mean.benefitRatio[i] ? (100 * v) / (oracle.mean.benefitRatio[i] as number) : null)),
    }));
  return (
    <ChartCard
      className="span-2"
      title="Empirical competitive ratio vs. the hindsight knapsack"
      sub="Benefit captured as a share of DP-Oracle* (100% = as good as an oracle that knows each epoch's true counts). Online algorithms have a theoretical worst-case ratio of k/(k−h+1) (Sleator-Tarjan); this shows the empirical ratio on realistic traffic."
    >
      <LineSeriesChart x={data.x} series={series} xLabel="cache size" logX xFmt={(v) => `${+(v * 100).toFixed(1)}%`} yFmt={(v) => `${v.toFixed(0)}%`} height={280} />
    </ChartCard>
  );
}
