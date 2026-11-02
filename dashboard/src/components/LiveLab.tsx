import { useEffect, useState } from 'react';
import { AggAlgo, METRICS, PATTERNS, PATTERN_INFO, RunFile, algoColor, fmt, metricByKey, scaleOf } from '../data';
import { ChartCard, LineSeriesChart, RankedBars, Select, useMode } from './ui';
import { CompareTable } from './Overview';

const ALGOS = [
  ['lru', 'LRU'], ['lfu', 'LFU'], ['gdsf', 'GDSF'], ['greedy-epoch', 'Greedy-Epoch'], ['dp-reactive', 'DP-Reactive'],
  ['dp-predictive', 'DP-Predictive'], ['pack', 'PACK-DP'], ['dp-oracle', 'DP-Oracle*'], ['belady', 'Belady*'],
];

export function LiveLab() {
  const mode = useMode();
  const [api, setApi] = useState<'checking' | 'up' | 'down'>('checking');
  const [form, setForm] = useState({ pattern: 'mixed', capacity: 0.05, alpha: 1, beta: 1, epochs: 20, epochLen: 10000, objects: 3000, seed: 7 });
  const [algos, setAlgos] = useState<string[]>(['lru', 'gdsf', 'dp-reactive', 'pack', 'dp-oracle']);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);
  const [run, setRun] = useState<RunFile | null>(null);
  const [metricKey, setMetricKey] = useState('benefitRatio');

  useEffect(() => {
    fetch('api/health').then((r) => setApi(r.ok ? 'up' : 'down')).catch(() => setApi('down'));
  }, []);

  const submit = async () => {
    setBusy(true);
    setErr(null);
    try {
      const r = await fetch('api/simulate', {
        method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ ...form, algos }),
      });
      const body = await r.json();
      if (!r.ok) throw new Error(body.error ?? `HTTP ${r.status}`);
      setRun(body as RunFile);
    } catch (e) {
      setErr((e as Error).message);
    } finally {
      setBusy(false);
    }
  };
  const num = (k: keyof typeof form, min: number, max: number, stepv: number, label: string) => (
    <label className="field">{label}
      <input type="number" min={min} max={max} step={stepv} value={form[k] as number} style={{ width: 96 }}
        onChange={(e) => setForm({ ...form, [k]: Math.max(min, Math.min(max, +e.target.value)) })} />
    </label>
  );
  const metric = metricByKey(metricKey);
  const asAgg: AggAlgo[] = run ? run.algorithms.map((a) => ({ key: a.key, name: a.name, oracle: a.oracle, mean: a.summary, std: {} })) : [];

  return (
    <>
      <div className="page-intro">
        <h2>Live lab: run the C++ engine</h2>
        <p>
          Configure a scenario and the Node bridge (<code>server/server.mjs</code>) runs the <code>cdnopt</code> binary and streams back its JSON.
          {api === 'down' && <> The API is <strong>not reachable</strong> (for example on the static GitHub Pages build). Start it locally with <code>npm start</code> in <code>server/</code>.</>}
        </p>
      </div>
      <ChartCard title="Scenario" sub={PATTERN_INFO[form.pattern]}>
        <div className="filters" style={{ marginBottom: 8 }}>
          <Select label="Traffic" options={PATTERNS.map((p) => ({ value: p, label: p }))} value={form.pattern} onChange={(v) => setForm({ ...form, pattern: v })} />
          {num('capacity', 0.002, 0.5, 0.005, 'Cache (fraction)')}
          {num('alpha', 0, 5, 0.1, 'α (bandwidth)')}
          {num('beta', 0, 5, 0.1, 'β (latency)')}
          {num('objects', 200, 20000, 100, 'Objects')}
          {num('epochs', 3, 60, 1, 'Epochs')}
          {num('epochLen', 1000, 50000, 1000, 'Requests/epoch')}
          {num('seed', 1, 9999, 1, 'Seed')}
        </div>
        <div className="filters" style={{ marginBottom: 8 }}>
          {ALGOS.map(([k, n]) => (
            <label className="field" key={k}>
              <input type="checkbox" checked={algos.includes(k)} onChange={(e) => setAlgos(e.target.checked ? [...algos, k] : algos.filter((a) => a !== k))} />
              <span className="key-line" style={{ borderTopColor: algoColor(k, mode) }} />{n}
            </label>
          ))}
        </div>
        <button className="btn btn-primary" onClick={submit} disabled={busy || api !== 'up' || algos.length === 0}>
          {busy ? 'Simulating…' : 'Run simulation'}
        </button>
        {err && <span className="down" style={{ marginLeft: 12 }}>{err}</span>}
      </ChartCard>
      {run && (
        <div className="grid grid-2" style={{ marginTop: 16, opacity: busy ? 0.5 : 1 }}>
          <ChartCard
            className="span-2"
            title={`${metric.label}: ${run.config.pattern}`}
            sub={`${run.catalog.objects} objects, ${run.catalog.totalGB.toFixed(1)} GB catalog, cache ${(run.capacityKB / 1048576).toFixed(2)} GB. Engine wall time ${(run.wallMs / 1000).toFixed(1)} s.`}
            controls={<Select label="Metric" options={METRICS.filter((m) => m.key !== 'burstHitRatio').map((m) => ({ value: m.key, label: m.label }))} value={metricKey} onChange={setMetricKey} />}
            table={() => <CompareTable algos={asAgg} />}
          >
            <RankedBars
              rows={run.algorithms.map((a) => ({ id: a.key, name: a.name, color: algoColor(a.key, mode), dashed: a.oracle, value: (a.summary[metricKey] as number) * scaleOf(metric) }))
                .sort((a, b) => (metric.higherBetter ? b.value - a.value : a.value - b.value))}
              fmt={(v) => (metric.pct ? `${v.toFixed(1)}%` : fmt(v, metric))}
            />
          </ChartCard>
          <ChartCard className="span-2" title="Benefit captured per epoch">
            <LineSeriesChart
              x={(run.algorithms[0].epochs?.benefitRatio ?? []).map((_, i) => i)}
              series={run.algorithms.map((a) => ({ id: a.key, name: a.name, color: algoColor(a.key, mode), dashed: a.oracle, values: (a.epochs?.benefitRatio ?? []).map((v) => v * 100) }))}
              xLabel="epoch" yFmt={(v) => `${v.toFixed(0)}%`} height={300} tipTitle={(l) => `Epoch ${l}`}
              areas={run.markers.map((m) => ({ x1: Math.max(0, m.epoch - 0.4), x2: m.epoch + (m.type === 'flash' ? 1.4 : 0.4), label: m.type }))}
            />
          </ChartCard>
        </div>
      )}
    </>
  );
}
