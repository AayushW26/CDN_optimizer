import { useState } from 'react';
import { OptGapFile, SolverBlock, SolversFile, slotColor, useJson } from '../data';
import { ChartCard, LineSeriesChart, Loading, useMode } from './ui';

// Solvers are their own entity set: fixed slot per solver name.
const SOLVER_SLOT: Record<string, number> = {
  dp2d: 1, rolling: 2, hirschberg: 0, bnb: 6, 'fptas:0.1': 4, greedy: 3, modgreedy: 7, mitm: 5,
};
const SOLVER_LABEL: Record<string, string> = {
  dp2d: '2-D DP table', rolling: '1-D DP + bit table', hirschberg: 'Hirschberg D&C DP', bnb: 'Branch & bound (MT-U2)',
  'fptas:0.1': 'FPTAS ε=0.1', greedy: 'Greedy (density)', modgreedy: 'Modified greedy', mitm: 'Meet-in-the-middle',
};

const fmtMs = (v: number) => (v >= 1000 ? `${(v / 1000).toFixed(1)} s` : v >= 1 ? `${v.toFixed(v < 10 ? 1 : 0)} ms` : `${(v * 1000).toFixed(0)} µs`);
const fmtKB = (v: number) => (v >= 1024 * 1024 ? `${(v / 1048576).toFixed(1)} GB` : v >= 1024 ? `${(v / 1024).toFixed(v < 10240 ? 1 : 0)} MB` : `${v.toFixed(0)} KB`);

function blockSeries(b: SolverBlock, field: 'ms' | 'memKB' | 'gapPct' | 'nodes', mode: 'light' | 'dark', only?: string[]) {
  return b.algorithms
    .filter((a) => !only || only.includes(a.name))
    .map((a) => ({
      id: a.name, name: SOLVER_LABEL[a.name] ?? a.name, color: slotColor(SOLVER_SLOT[a.name] ?? 7, mode),
      values: a[field].map((v) => (v === null || v <= 0 ? null : v)),
    }));
}

export function Algorithms() {
  const { data, error } = useJson<SolversFile>('solvers.json');
  const opt = useJson<OptGapFile>('optgap.json');
  const [axis, setAxis] = useState<'scalingN' | 'scalingC'>('scalingN');
  const [measure, setMeasure] = useState<'ms' | 'memKB'>('ms');
  const mode = useMode();
  if (!data) return <Loading error={error} />;
  const block = data[axis];
  const slopeRows = block.algorithms.filter((a) => a.logLogSlope !== null);

  return (
    <>
      <div className="page-intro">
        <h2>DAA lab: the knapsack solvers</h2>
        <p>
          Nine solvers share one interface and are benchmarked on the same instances (median of 3 runs). Cache-like instances
          have Zipf values and log-normal sizes; <em>hard</em> instances are Pisinger's strongly-correlated family
          (v = w + R/10), where bounds are weak.
        </p>
      </div>
      <div className="grid grid-2">
        <ChartCard
          className="span-2"
          title={`${measure === 'ms' ? 'Running time' : 'Table memory'} vs ${axis === 'scalingN' ? 'n (items)' : 'C (capacity units)'}`}
          sub={`${block.variable}. Log-log axes: a straight line of slope k means time ∝ x^k. Missing points were skipped because they exceed 700 MB of memory.`}
          controls={
            <>
              <div className="seg">{(['scalingN', 'scalingC'] as const).map((a) => <button key={a} aria-pressed={axis === a} onClick={() => setAxis(a)}>{a === 'scalingN' ? 'vary n' : 'vary C'}</button>)}</div>
              <div className="seg">{(['ms', 'memKB'] as const).map((a) => <button key={a} aria-pressed={measure === a} onClick={() => setMeasure(a)}>{a === 'ms' ? 'time' : 'memory'}</button>)}</div>
            </>
          }
          table={() => (
            <table>
              <thead><tr><th>Solver</th>{block.x.map((x) => <th key={x}>{x}</th>)}<th>slope</th></tr></thead>
              <tbody>{block.algorithms.map((a) => (
                <tr key={a.name}><td>{SOLVER_LABEL[a.name] ?? a.name}</td>
                  {a[measure].map((v, i) => <td key={i}>{v === null ? '–' : measure === 'ms' ? fmtMs(v) : fmtKB(v)}</td>)}
                  <td>{a.logLogSlope?.toFixed(2) ?? '–'}</td></tr>
              ))}</tbody>
            </table>
          )}
          note={
            <>
              <strong>Reading it:</strong> all three exact DPs are Θ(nC) in time, but memory separates them. The 2-D table
              needs n·C doubles, the rolling DP needs C doubles plus an n·C <em>bit</em> table, and the Hirschberg-style
              divide &amp; conquer needs only O(n + C) by recomputing halves. It stays at a few hundred KB while the 2-D table reaches 512 MB.
              Empirical log-log slopes: {slopeRows.map((a) => `${SOLVER_LABEL[a.name] ?? a.name} ${a.logLogSlope!.toFixed(2)}`).join(' · ')}.
            </>
          }
        >
          <LineSeriesChart x={block.x} series={blockSeries(block, measure, mode)} xLabel={axis === 'scalingN' ? 'n' : 'C'} logX logY
            yFmt={measure === 'ms' ? fmtMs : fmtKB} xFmt={(v) => (v >= 1000 ? `${v / 1000}k` : String(v))} height={340} />
        </ChartCard>

        <ChartCard
          title="Hard instances: B&B goes exponential"
          sub="Strongly-correlated items. B&B nodes explored (log scale, capped at 5×10⁷) vs the DP's time in ms."
          table={() => (
            <table>
              <thead><tr><th>n</th>{data.hardCorrelated.algorithms.map((a) => <th key={a.name}>{SOLVER_LABEL[a.name]} ms</th>)}<th>B&B nodes</th><th>greedy gap</th></tr></thead>
              <tbody>{data.hardCorrelated.x.map((x, i) => (
                <tr key={x}><td>{x}</td>{data.hardCorrelated.algorithms.map((a) => <td key={a.name}>{a.ms[i] === null ? '–' : fmtMs(a.ms[i]!)}</td>)}
                  <td>{data.hardCorrelated.algorithms.find((a) => a.name === 'bnb')?.nodes[i]?.toLocaleString()}</td>
                  <td>{data.hardCorrelated.algorithms.find((a) => a.name === 'greedy')?.gapPct[i]?.toFixed(2)}%</td></tr>
              ))}</tbody>
            </table>
          )}
        >
          <LineSeriesChart x={data.hardCorrelated.x} series={blockSeries(data.hardCorrelated, 'ms', mode)} xLabel="n" logY yFmt={fmtMs} height={280} />
        </ChartCard>

        <ChartCard
          title="Huge capacity: meet-in-the-middle"
          sub="Item sizes up to 10⁹ bytes. A DP table would need C ≈ 10¹⁰ columns, which is infeasible. MITM is O(2^{n/2}·n) and independent of C."
          table={() => (
            <table>
              <thead><tr><th>n</th><th>MITM time</th><th>MITM memory</th><th>B&B time</th></tr></thead>
              <tbody>{data.hugeCapacity.x.map((x, i) => {
                const m = data.hugeCapacity.algorithms.find((a) => a.name === 'mitm')!;
                const b = data.hugeCapacity.algorithms.find((a) => a.name === 'bnb')!;
                return <tr key={x}><td>{x}</td><td>{m.ms[i] === null ? '–' : fmtMs(m.ms[i]!)}</td><td>{m.memKB[i] === null ? '–' : fmtKB(m.memKB[i]!)}</td><td>{b.ms[i] === null ? '–' : fmtMs(b.ms[i]!)}</td></tr>;
              })}</tbody>
            </table>
          )}
        >
          <LineSeriesChart x={data.hugeCapacity.x} series={blockSeries(data.hugeCapacity, 'ms', mode, ['mitm', 'bnb'])} xLabel="n" logY yFmt={fmtMs} height={280} />
        </ChartCard>

        <ChartCard
          title="FPTAS: accuracy is a dial"
          sub="Guaranteed bound (1−ε)·OPT vs the gap actually observed, on a cache instance and a hard instance. Time is O(n²/ε), independent of C."
          table={() => (
            <table>
              <thead><tr><th>ε</th><th>guarantee</th><th>gap (cache)</th><th>gap (hard)</th><th>time</th><th>memory</th></tr></thead>
              <tbody>{data.fptasEps.eps.map((e, i) => (
                <tr key={e}><td>{e}</td><td>≤ {(e * 100).toFixed(0)}%</td><td>{data.fptasEps.gapPct[i].toFixed(4)}%</td><td>{data.fptasEps.gapPctHard[i].toFixed(3)}%</td>
                  <td>{fmtMs(data.fptasEps.ms[i])}</td><td>{fmtKB(data.fptasEps.memKB[i])}</td></tr>
              ))}</tbody>
            </table>
          )}
        >
          <LineSeriesChart
            x={data.fptasEps.eps}
            series={[
              { id: 'g', name: 'guaranteed max gap (ε)', color: slotColor(1, mode), values: data.fptasEps.eps.map((e) => e * 100) },
              { id: 'h', name: 'observed gap, hard instance', color: slotColor(6, mode), values: data.fptasEps.gapPctHard.map((v) => (v > 0 ? v : null)) },
              { id: 'c', name: 'observed gap, cache instance', color: slotColor(0, mode), values: data.fptasEps.gapPct.map((v) => (v > 0 ? v : null)) },
            ]}
            xLabel="ε" logX logY yFmt={(v) => `${v < 0.01 ? v.toExponential(0) : v.toFixed(v < 1 ? 2 : 0)}%`} height={280}
          />
        </ChartCard>

        <ChartCard
          title="Time / accuracy frontier of FPTAS"
          sub="Halving ε roughly doubles the work: the O(n²/ε) bound in action."
        >
          <LineSeriesChart x={data.fptasEps.eps} series={[{ id: 't', name: 'FPTAS time', color: slotColor(4, mode), values: data.fptasEps.ms }]}
            xLabel="ε" logX logY yFmt={fmtMs} height={280} />
        </ChartCard>

        {opt.data && <OptGap d={opt.data} />}
        <ComplexityTable />
      </div>
    </>
  );
}

function OptGap({ d }: { d: OptGapFile }) {
  const mode = useMode();
  const [stat, setStat] = useState<'mean' | 'max'>('max');
  const label: Record<string, string> = {
    greedy: 'Greedy (density)', modgreedy: 'Modified greedy', 'fptas:0.05': 'FPTAS ε=0.05', 'unitDP-4096': 'Unit DP, no fill', 'unitDP-4096+fill': 'Unit DP + slack fill',
  };
  const slot: Record<string, number> = { greedy: 3, modgreedy: 7, 'fptas:0.05': 4, 'unitDP-4096': 1, 'unitDP-4096+fill': 2 };
  const series = d.methods.filter((m) => m.name !== 'modgreedy').map((m) => ({
    id: m.name, name: label[m.name] ?? m.name, color: slotColor(slot[m.name] ?? 0, mode),
    values: (stat === 'mean' ? m.meanGapPct : m.maxGapPct).map((v) => Math.max(v, 1e-3)),
  }));
  series.push({ id: 'bound', name: 'Bound v_break / OPT (mean)', color: slotColor(5, mode), values: d.boundPct.map((v) => Math.max(v, 1e-3)) });
  return (
    <ChartCard
      className="span-2"
      title="Price of approximation on real cache instances"
      sub={`Optimality gap vs the best known solution on ${d.instances} instances (true request counts of a Zipf epoch, ~${Math.round(d.items[0])} objects, real byte sizes). The LP (Dantzig) bound certifies the best known is within ${Math.max(...d.lpSlackMaxPct).toFixed(2)}% of the true optimum.`}
      controls={<div className="seg">{(['max', 'mean'] as const).map((s) => <button key={s} aria-pressed={stat === s} onClick={() => setStat(s)}>{s === 'max' ? 'worst instance' : 'mean'}</button>)}</div>}
      table={() => (
        <table>
          <thead><tr><th>cache size</th>{d.methods.map((m) => <th key={m.name}>{label[m.name]} (mean / max)</th>)}<th>bound</th><th>LP slack (max)</th><th>B&B proved</th></tr></thead>
          <tbody>{d.x.map((x, i) => (
            <tr key={x}><td>{(x * 100).toFixed(1)}%</td>
              {d.methods.map((m) => <td key={m.name}>{m.meanGapPct[i].toFixed(3)} / {m.maxGapPct[i].toFixed(2)}%</td>)}
              <td>{d.boundPct[i].toFixed(2)}%</td><td>{d.lpSlackMaxPct[i].toFixed(3)}%</td><td>{(d.provenFraction[i] * 100).toFixed(0)}%</td></tr>
          ))}</tbody>
        </table>
      )}
      note={
        <>
          <strong>Takeaways.</strong> (1) Greedy's gap is provably at most v<sub>break</sub>/OPT. It is tiny in large caches
          but reaches double digits in tiny caches, where a few large objects decide the optimum. That is when exact DP pays
          off (compare DP-Reactive and Greedy at 0.5% in the capacity sweep). (2) Rounding sizes to C units costs up to ~9% of
          value unless the leftover room is refilled in exact bytes. (3) These gaps are still smaller than the
          <em> forecast</em> gap (DP-Reactive vs DP-Oracle* ≈ 5 pts), which is why PACK-DP invests in prediction.
        </>
      }
    >
      <LineSeriesChart x={d.x} series={series} xLabel="cache size (fraction of catalog)" logX logY xFmt={(v) => `${+(v * 100).toFixed(1)}%`}
        yFmt={(v) => (v < 0.01 ? '≤0.001%' : `${v.toFixed(v < 1 ? 2 : 1)}%`)} height={320} />
    </ChartCard>
  );
}

function ComplexityTable() {
  const rows = [
    ['2-D DP table', 'O(nC)', 'O(nC)', 'exact', 'reference; reconstruct by comparing rows'],
    ['1-D rolling DP + bit table', 'O(nC)', 'O(C) + nC bits', 'exact', '64× less memory than doubles'],
    ['Hirschberg D&C DP', 'O(nC)', 'O(n + C)', 'exact', 'split items, best capacity split c*, recurse; Σ work ≤ 2nC'],
    ['Branch & bound (Martello-Toth U2)', 'O(2ⁿ) worst', 'O(n)', 'exact', 'C-independent; fast unless values ∝ weights'],
    ['Meet-in-the-middle', 'O(2^{n/2} · n)', 'O(2^{n/2})', 'exact', 'n ≤ 44; capacity can be 10¹⁰+'],
    ['FPTAS (profit scaling)', 'O(n² / ε)', 'O(n² / ε) bits', '≥ (1−ε)·OPT', 'K = ε·LB/n with LB = modified greedy'],
    ['Modified greedy', 'O(n log n)', 'O(n)', '≥ OPT / 2', 'max(greedy, best single item)'],
    ['Multiple-choice KP DP', 'O(C · Σ|gᵢ|)', 'O(C + G·C)', 'exact', 'multi-bitrate video (one rendition per title)'],
    ['LRU / O(1) LFU', 'O(1) per request', 'O(k)', 'k/(k−h+1)-competitive (LRU)', 'online, no prediction'],
    ['GDSF', 'O(log k) per request', 'O(k)', 'heuristic', 'priority = L + f·B/s'],
    ['Size-aware Belady*', 'O(log k) per request', 'O(N) future', 'optimal for unit sizes', 'needs the future (oracle)'],
    ['PACK-DP (per epoch)', 'O(n·E) Hedge + KP solve', 'O(n·E)', 'exact plan for its forecast', 'E = 4 forecasting experts'],
  ];
  return (
    <ChartCard className="span-2" title="Complexity summary" sub="n = candidate objects, C = capacity units, k = cached objects, G = groups (videos), E = experts.">
      <div className="table-wrap">
        <table>
          <thead><tr><th>Algorithm</th><th>Time</th><th>Space</th><th>Guarantee</th><th style={{ textAlign: 'left' }}>Notes</th></tr></thead>
          <tbody>{rows.map((r) => <tr key={r[0]}><td>{r[0]}</td><td>{r[1]}</td><td>{r[2]}</td><td>{r[3]}</td><td style={{ textAlign: 'left' }}>{r[4]}</td></tr>)}</tbody>
        </table>
      </div>
    </ChartCard>
  );
}
