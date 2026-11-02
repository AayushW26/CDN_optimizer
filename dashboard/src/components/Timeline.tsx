import { useMemo, useState } from 'react';
import { Bar, BarChart, CartesianGrid, ResponsiveContainer, Tooltip, XAxis, YAxis } from 'recharts';
import { CHROME, PATTERN_INFO, PATTERNS, RunFile, algoColor, slotColor, useJson } from '../data';
import { ChartCard, Legend, LineSeriesChart, Loading, Seg, Select, useHidden, useMode } from './ui';

const SERIES_METRICS = [
  { key: 'benefitRatio', label: 'Benefit captured', pct: true },
  { key: 'hitRatio', label: 'Hit ratio', pct: true },
  { key: 'byteHitRatio', label: 'Byte hit ratio', pct: true },
  { key: 'latencyMs', label: 'Mean latency (ms)', pct: false },
  { key: 'originMB', label: 'Origin traffic (MB, incl. prefetch)', pct: false },
  { key: 'planMs', label: 'Planning time (ms)', pct: false },
];
const KINDS = ['Static (images, JS)', 'Video segments', 'Large files'];

export function Timeline() {
  const [pattern, setPattern] = useState('flash');
  const [metric, setMetric] = useState('benefitRatio');
  const { data, error } = useJson<RunFile>(`timeline_${pattern}.json`);
  const mode = useMode();
  const [logAlgo, setLogAlgo] = useState('pack');

  const view = useMemo(() => {
    if (!data) return null;
    const m = SERIES_METRICS.find((s) => s.key === metric)!;
    const n = data.algorithms[0]?.epochs?.[metric]?.length ?? 0;
    const x = Array.from({ length: n }, (_, i) => i);
    const series = data.algorithms.map((a) => ({
      id: a.key, name: a.name, color: algoColor(a.key, mode), dashed: a.oracle,
      values: (a.epochs?.[metric] ?? []).map((v) => (m.pct ? v * 100 : v)),
    }));
    // each epoch is plotted as one point: shade the onset epoch (and the next for flash crowds)
    const areas = data.markers.map((mk) => ({
      x1: Math.max(0, mk.epoch - 0.4),
      x2: Math.min(n - 1, mk.epoch + (mk.type === 'flash' ? 1.4 : 0.4)),
      label: mk.type === 'flash' ? 'flash' : 'scan',
    }));
    return { m, x, series, areas };
  }, [data, metric, mode]);

  if (!data || !view) return <Loading error={error} />;
  const planners = data.algorithms.filter((a) => a.events && a.events.length);
  const logOf = planners.find((a) => a.key === logAlgo) ?? planners[0];
  const yFmt = (v: number) => (view.m.pct ? `${v.toFixed(0)}%` : v >= 1000 ? `${(v / 1000).toFixed(1)}k` : v.toFixed(v < 10 ? 1 : 0));

  return (
    <>
      <div className="page-intro">
        <h2>Epoch-by-epoch behaviour</h2>
        <p>
          One representative run (seed 1). Shaded bands mark flash crowds and crawler scans. Watch the planners at a flash-crowd
          onset: DP-Reactive only reacts an epoch later, while PACK-DP's burst detector re-plans <em>within</em> the epoch.
        </p>
      </div>
      <div className="filters">
        <Seg label="Traffic" options={PATTERNS.map((p) => ({ value: p, label: p }))} value={pattern} onChange={setPattern} />
        <Select label="Metric" options={SERIES_METRICS.map((s) => ({ value: s.key, label: s.label }))} value={metric} onChange={setMetric} />
      </div>
      <div className="grid grid-2">
        <ChartCard
          className="span-2"
          title={`${view.m.label} per epoch: ${pattern}`}
          sub={`${PATTERN_INFO[pattern]} Click legend entries to hide series.`}
          table={() => (
            <table>
              <thead><tr><th>Epoch</th>{view.series.map((s) => <th key={s.id}>{s.name}</th>)}</tr></thead>
              <tbody>
                {view.x.map((e) => (
                  <tr key={e}><td>{e}</td>{view.series.map((s) => <td key={s.id}>{s.values[e] !== undefined ? yFmt(s.values[e] as number) : '–'}</td>)}</tr>
                ))}
              </tbody>
            </table>
          )}
        >
          <LineSeriesChart
            x={view.x} series={view.series} xLabel="epoch" yFmt={yFmt} height={340} areas={view.areas}
            hiddenInit={['lfu', 'greedy-epoch']} tipTitle={(l) => `Epoch ${l}`}
          />
        </ChartCard>

        <ChartCard
          title="Knapsack plan log"
          sub="Every (re-)plan: solver used, candidates, value, and the objects admitted / evicted (ids)."
          controls={
            <select value={logOf?.key} onChange={(e) => setLogAlgo(e.target.value)} aria-label="Planner">
              {planners.map((a) => <option key={a.key} value={a.key}>{a.name}</option>)}
            </select>
          }
        >
          <div className="event-log">
            {logOf?.events?.slice(0, 80).map((ev, i) => (
              <div className="event" key={i}>
                <div>
                  <strong>Epoch {ev.epoch}</strong>
                  <span className={`badge ${ev.reason !== 'epoch' ? 'badge-accent' : ''}`}>{ev.reason === 'epoch' ? 'scheduled' : `${ev.reason} re-plan`}</span>
                  <span className="badge">{ev.solver}</span>
                </div>
                <div className="event-meta">
                  {ev.candidates} candidates · solved in {ev.solveMs.toFixed(1)} ms · plan {ev.plannedMB.toFixed(0)} MB ·
                  expected hit {(ev.expectedHitRatio * 100).toFixed(1)}%
                </div>
                {(ev.admitted.length > 0 || ev.evicted.length > 0) && (
                  <div className="chips">
                    {ev.admitted.slice(0, 14).map((id) => <span key={'a' + id} className="chip add">+{id}</span>)}
                    {ev.evicted.slice(0, 14).map((id) => <span key={'e' + id} className="chip rm">−{id}</span>)}
                  </div>
                )}
              </div>
            ))}
          </div>
        </ChartCard>

        <Composition data={data} />
      </div>
    </>
  );
}

function Composition({ data }: { data: RunFile }) {
  const mode = useMode();
  const c = CHROME[mode];
  const [by, setBy] = useState<'count' | 'kb'>('count');
  const { hidden, toggle } = useHidden();
  const kindColors = [slotColor(2, mode), slotColor(3, mode), slotColor(6, mode)];
  const rows = data.algorithms
    .filter((a) => (a as unknown as { composition?: unknown }).composition)
    .map((a) => {
      const comp = (a as unknown as { composition: { count: number[]; kb: number[] } }).composition;
      const v = comp[by];
      const tot = v.reduce((s, x) => s + x, 0) || 1;
      return { name: a.name, k0: (100 * v[0]) / tot, k1: (100 * v[1]) / tot, k2: (100 * v[2]) / tot, total: by === 'count' ? tot : tot / 1048576 };
    });
  const keys = ['k0', 'k1', 'k2'];
  return (
    <ChartCard
      title="What ends up in the cache"
      sub={`Final cache contents by content kind (share of ${by === 'count' ? 'objects' : 'bytes'}).`}
      controls={<div className="seg">{(['count', 'kb'] as const).map((b) => <button key={b} aria-pressed={by === b} onClick={() => setBy(b)}>{b === 'count' ? 'objects' : 'bytes'}</button>)}</div>}
      table={() => (
        <table>
          <thead><tr><th>Algorithm</th>{KINDS.map((k) => <th key={k}>{k}</th>)}<th>{by === 'count' ? 'objects' : 'GB'}</th></tr></thead>
          <tbody>{rows.map((r) => <tr key={r.name}><td>{r.name}</td><td>{r.k0.toFixed(1)}%</td><td>{r.k1.toFixed(1)}%</td><td>{r.k2.toFixed(1)}%</td><td>{r.total.toFixed(by === 'count' ? 0 : 2)}</td></tr>)}</tbody>
        </table>
      )}
    >
      {rows.length === 0 ? (
        <div className="empty">Re-run <code>cdnopt experiment compare</code> to export cache composition.</div>
      ) : (
        <>
          <Legend kind="rect" series={keys.map((k, i) => ({ id: k, name: KINDS[i], color: kindColors[i] }))} hidden={hidden} onToggle={toggle} />
          <ResponsiveContainer width="100%" height={rows.length * 30 + 30}>
            <BarChart data={rows} layout="vertical" margin={{ top: 0, right: 12, bottom: 0, left: 8 }}>
              <CartesianGrid stroke={c.grid} horizontal={false} />
              <XAxis type="number" domain={[0, 100]} ticks={[0, 25, 50, 75, 100]} tickFormatter={(v) => `${Math.round(v)}%`} stroke={c.axis} tick={{ fill: c.muted, fontSize: 11.5 }} tickLine={false} />
              <YAxis type="category" dataKey="name" width={140} tick={{ fill: c.ink2, fontSize: 12 }} tickLine={false} axisLine={false} />
              <Tooltip
                cursor={{ fill: c.grid, fillOpacity: 0.5 }}
                content={({ active, payload, label }) =>
                  active && payload?.length ? (
                    <div className="tip">
                      <div className="tip-title">{label}</div>
                      {payload.map((p) => (
                        <div className="tip-row" key={String(p.dataKey)}>
                          <span className="key-rect" style={{ background: p.color }} />
                          <span className="v">{(p.value as number).toFixed(1)}%</span>
                          <span className="n">{KINDS[keys.indexOf(String(p.dataKey))]}</span>
                        </div>
                      ))}
                    </div>
                  ) : null
                }
              />
              {keys.filter((k) => !hidden.has(k)).map((k) => (
                <Bar key={k} dataKey={k} stackId="a" fill={kindColors[keys.indexOf(k)]} barSize={18} stroke={c.surface} strokeWidth={2} isAnimationActive={false} />
              ))}
            </BarChart>
          </ResponsiveContainer>
        </>
      )}
    </ChartCard>
  );
}
