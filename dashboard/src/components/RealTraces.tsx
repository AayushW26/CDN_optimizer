import { useMemo, useState } from 'react';
import { CHROME, useJson } from '../data';
import {
  BASELINES, LAB_TRACES, LabFile, LabMetric, bestBaseline, byPolicy, gb, labColor, millions, pctSize, ratio, sizesOf,
} from '../lab';
import { ChartCard, LineSeriesChart, Loading, RankedBars, Seg, SeriesDef, Stat, useMode } from './ui';

type TraceId = (typeof LAB_TRACES)[number]['id'];

const CURVE_POLICIES = ['SHADE', 'GDSF', 'LHD', 'AdaptSize', 'W-TinyLFU', 'S3-FIFO', 'SIEVE', 'LRU'];
const HIDDEN_AT_START = ['SIEVE', 'W-TinyLFU'];

export function RealTraces() {
  const mode = useMode();
  const c = CHROME[mode];
  const [trace, setTrace] = useState<TraceId>('w18test');
  const [metric, setMetric] = useState<LabMetric>('objectMissRatio');
  const [size, setSize] = useState(0.01);

  const w18 = useJson<LabFile>('lab/w18test_main.json');
  const w19u = useJson<LabFile>('lab/w19upload_main.json');
  const w19t = useJson<LabFile>('lab/w19text_main.json');
  const dev = useJson<LabFile>('lab/w18dev_main.json');
  const files: Record<TraceId, LabFile | null> = { w18test: w18.data, w19upload: w19u.data, w19text: w19t.data, w18dev: dev.data };
  const f = files[trace];
  const shadeName = metric === 'byteMissRatio' ? 'SHADE-B' : 'SHADE';

  // Headline: across every held-out trace and cache size, how often does SHADE beat the best baseline?
  const headline = useMemo(() => {
    const held = [files.w18test, files.w19upload, files.w19text].filter(Boolean) as LabFile[];
    let wins = 0, total = 0, sumLru = 0;
    const small: number[] = [], large: number[] = [];
    for (const hf of held) {
      const t = byPolicy(hf, 'objectMissRatio');
      for (const s of sizesOf(hf)) {
        const sh = t.get('SHADE')?.get(s);
        const lru = t.get('LRU')?.get(s);
        if (sh === undefined || lru === undefined) continue;
        const bb = bestBaseline(t, s);
        const imp = (bb.value - sh) / bb.value;
        total++;
        if (sh < bb.value) wins++;
        (s <= 0.01 ? small : large).push(imp);
        sumLru += (lru - sh) / lru;
      }
    }
    const mean = (a: number[]) => (a.length ? a.reduce((x, y) => x + y, 0) / a.length : 0);
    return { wins, total, small: mean(small), nSmall: small.length, large: mean(large), nLarge: large.length, meanLru: total ? sumLru / total : 0, traces: held.length };
  }, [files.w18test, files.w19upload, files.w19text]);

  if (!f) return <Loading error={[w18, w19u, w19t, dev].find((x) => x.error)?.error ?? null} />;
  const t = byPolicy(f, metric);
  const sizes = sizesOf(f);
  const sel = sizes.includes(size) ? size : sizes[Math.min(2, sizes.length - 1)];
  const bound = new Map(f.bounds.map((b) => [b.cacheFrac, b[metric]]));
  const info = LAB_TRACES.find((x) => x.id === trace)!;

  const series: SeriesDef[] = [
    ...CURVE_POLICIES.map((p) => (p === 'SHADE' ? shadeName : p)).filter((p) => t.has(p)).map((p) => ({
      id: p, name: p === 'SHADE-B' ? 'SHADE-B (byte objective)' : p, color: labColor(p, mode), values: sizes.map((s) => t.get(p)!.get(s) ?? null),
    })),
    { id: 'belady', name: 'Belady-Size* (offline)', color: c.oracleA, dashed: true, values: sizes.map((s) => t.get('Belady-Size*')?.get(s) ?? null) },
    { id: 'pfoo', name: 'PFOO-L bound', color: c.oracleB, dashed: true, values: sizes.map((s) => bound.get(s) ?? null) },
  ];

  const ranked = [...BASELINES, shadeName]
    .filter((p) => t.get(p)?.has(sel))
    .map((p) => ({ id: p, name: p, value: t.get(p)!.get(sel)!, color: p === shadeName ? labColor('SHADE', mode) : c.axis }))
    .sort((a, b) => a.value - b.value);
  const best = bestBaseline(t, sel);
  const shadeSel = t.get(shadeName)?.get(sel);
  const metricWord = metric === 'objectMissRatio' ? 'misses' : 'missed bytes';

  return (
    <div className="page">
      <div className="page-head">
        <h2>SHADE on real CDN traffic</h2>
        <p>
          Every policy replays the same real request log from Wikipedia’s and Wikimedia’s CDN. SHADE was designed and its one constant
          tuned on the <i>dev</i> slice only, then frozen and committed before the three held-out traces were run.
          Lower miss ratio is better; the dashed lines are offline references that know the future.
        </p>
      </div>

      <div className="stats">
        <Stat label="Held-out settings where SHADE beats all 8 baselines" value={`${headline.wins} / ${headline.total}`}
          delta={`${headline.traces} trace${headline.traces === 1 ? '' : 's'} × 5 cache sizes`} deltaGood={null} />
        <Stat label="Small caches (≤ 1%): fewer misses than the best baseline" value={`${(100 * headline.small).toFixed(1)}%`}
          delta={<>large caches (3–10%): {headline.large >= 0 ? `${(100 * headline.large).toFixed(1)}% fewer` : `${(-100 * headline.large).toFixed(1)}% more`} misses</>}
          deltaGood={null} />
        <Stat label="Fewer misses than LRU (mean)" value={`${(100 * headline.meanLru).toFixed(1)}%`} delta="object miss ratio" deltaGood={null} />
        <Stat label="This trace" value={millions(f.requests) + ' requests'}
          delta={`${millions(f.objects)} objects · ${gb(f.footprintBytes)} · ${(100 * f.oneHitObjectFrac).toFixed(0)}% requested once`} deltaGood={null} />
      </div>

      <div className="controls">
        <Seg label="Trace" value={trace} onChange={setTrace} options={LAB_TRACES.map((x) => ({ value: x.id, label: x.label + (x.heldOut ? '' : ' (dev)') }))} />
        <Seg label="Metric" value={metric} onChange={setMetric} options={[{ value: 'objectMissRatio', label: 'Object misses' }, { value: 'byteMissRatio', label: 'Byte misses' }]} />
      </div>

      <ChartCard
        title={`${metric === 'objectMissRatio' ? 'Object' : 'Byte'} miss ratio vs cache size · ${info.label}`}
        sub={<>{info.sub}. Cache size is a share of the trace footprint (all distinct objects). Click a legend entry to show or hide a policy.</>}
        table={() => (
          <table>
            <thead><tr><th>policy</th>{sizes.map((s) => <th key={s} className="num">{pctSize(s)}</th>)}</tr></thead>
            <tbody>
              {[...t.keys()].map((p) => (
                <tr key={p}><td>{p}</td>{sizes.map((s) => <td key={s} className="num">{t.get(p)!.get(s) !== undefined ? ratio(t.get(p)!.get(s)!) : '–'}</td>)}</tr>
              ))}
              <tr><td>PFOO-L bound</td>{sizes.map((s) => <td key={s} className="num">{ratio(bound.get(s)!)}</td>)}</tr>
            </tbody>
          </table>
        )}
      >
        <LineSeriesChart x={sizes} series={series} logX xFmt={pctSize} yFmt={(v) => v.toFixed(2)} xLabel="cache size" height={340}
          hiddenInit={HIDDEN_AT_START} tipTitle={(l) => `cache ${pctSize(Number(l))}`} />
      </ChartCard>

      <div className="grid-2">
        <ChartCard
          title={`Ranking at ${pctSize(sel)} cache`}
          sub={shadeSel !== undefined ? <>SHADE{metric === 'byteMissRatio' ? '-B' : ''}: {ratio(shadeSel)} vs best baseline {best.name}: {ratio(best.value)} → <b>{(100 * (best.value - shadeSel) / best.value).toFixed(1)}% fewer {metricWord}</b></> : undefined}
          controls={<Seg value={sel} onChange={setSize} options={sizes.map((s) => ({ value: s, label: pctSize(s) }))} />}
        >
          <RankedBars rows={ranked} fmt={ratio} />
        </ChartCard>
        <ShadeVsBest files={files} metric={metric} />
      </div>

      <AblationCard />
    </div>
  );
}

function ShadeVsBest({ files, metric }: { files: Record<string, LabFile | null>; metric: LabMetric }) {
  const shade = metric === 'byteMissRatio' ? 'SHADE-B' : 'SHADE';
  const rows: { trace: string; size: number; sh: number; bb: number; name: string }[] = [];
  for (const tr of LAB_TRACES.filter((x) => x.heldOut)) {
    const f = files[tr.id];
    if (!f) continue;
    const t = byPolicy(f, metric);
    for (const s of sizesOf(f)) {
      const sh = t.get(shade)?.get(s);
      if (sh === undefined) continue;
      const bb = bestBaseline(t, s);
      rows.push({ trace: tr.label, size: s, sh, bb: bb.value, name: bb.name });
    }
  }
  return (
    <ChartCard title={`${shade} vs the best baseline, every held-out setting`} sub="The best baseline differs by trace and size; SHADE is compared with whichever is best.">
      <div className="table-wrap">
        <table>
          <thead><tr><th>trace</th><th className="num">cache</th><th className="num">{shade}</th><th className="num">best baseline</th><th className="num">change</th></tr></thead>
          <tbody>
            {rows.map((r) => {
              const imp = (r.bb - r.sh) / r.bb;
              return (
                <tr key={r.trace + r.size}>
                  <td>{r.trace}</td>
                  <td className="num">{pctSize(r.size)}</td>
                  <td className="num">{ratio(r.sh)}</td>
                  <td className="num">{ratio(r.bb)} <span className="muted">{r.name}</span></td>
                  <td className={`num ${imp > 0 ? 'up' : 'down'}`}>{imp > 0 ? '−' : '+'}{Math.abs(100 * imp).toFixed(1)}% {imp > 0 ? 'better' : 'worse'}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </ChartCard>
  );
}

const ABLATIONS: [string, string][] = [
  ['SHADE-noAdmit', 'No admission control'],
  ['SHADE:exchange=0', 'Threshold instead of exchange test'],
  ['SHADE:slack=1', 'Exchange margin 1 (untuned)'],
  ['SHADE:index=0:h=4', 'Fixed horizon instead of index'],
  ['SHADE:km=0', 'No censoring correction'],
  ['SHADE:shrink=0', 'No shrinkage'],
  ['SHADE:pool=0', 'No eviction pool'],
  ['SHADE-noGap', 'No gap feature'],
  ['SHADE-noCount', 'No count feature'],
  ['SHADE-noFeatures', 'No features'],
];

function AblationCard() {
  const mode = useMode();
  const c = CHROME[mode];
  const [trace, setTrace] = useState<'w18test' | 'w19upload' | 'w19text'>('w18test');
  const [size, setSize] = useState(0.01);
  const { data, error } = useJson<LabFile>(`lab/${trace}_ablation.json`);
  if (!data) return <ChartCard title="Ablations"><Loading error={error} /></ChartCard>;
  const t = byPolicy(data, 'objectMissRatio');
  const sizes = sizesOf(data);
  const sel = sizes.includes(size) ? size : sizes[0];
  const base = t.get('SHADE')!.get(sel)!;
  const rows = ABLATIONS.filter(([p]) => t.get(p)?.has(sel)).map(([p, name]) => {
    const v = t.get(p)!.get(sel)!;
    return { id: p, name, value: (100 * (v - base)) / base, color: v > base ? labColor('SHADE', mode) : c.axis };
  });
  return (
    <ChartCard
      title="What each part of SHADE contributes"
      sub={`Change in object miss ratio when one component is removed (positive = more misses = the component helps). Full SHADE: ${ratio(base)}.`}
      controls={
        <div className="controls tight">
          <Seg value={trace} onChange={setTrace} options={[{ value: 'w18test', label: 'Wiki 2018' }, { value: 'w19upload', label: '2019 upload' }, { value: 'w19text', label: '2019 text' }]} />
          <Seg value={sel} onChange={setSize} options={sizes.map((s) => ({ value: s, label: pctSize(s) }))} />
        </div>
      }
    >
      <RankedBars rows={rows} fmt={(v) => `${v >= 0 ? '+' : ''}${v.toFixed(1)}%`} />
    </ChartCard>
  );
}
