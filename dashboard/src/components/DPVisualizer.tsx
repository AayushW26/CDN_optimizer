import { useEffect, useMemo, useState } from 'react';
import { ChartCard } from './ui';

interface Item { name: string; w: number; v: number }

const PRESETS: Record<string, { items: Item[]; C: number; note: string }> = {
  'CDN mini': {
    C: 10,
    note: 'Six edge objects. Sizes in 100 MB units; benefit = requests/epoch × (cost + latency weight).',
    items: [
      { name: 'hero.jpg', w: 1, v: 9 },
      { name: 'promo.mp4', w: 4, v: 20 },
      { name: 'app.js', w: 2, v: 12 },
      { name: 'trailer.mp4', w: 5, v: 24 },
      { name: 'patch.bin', w: 6, v: 33 },
      { name: 'thumbs.zip', w: 3, v: 13 },
    ],
  },
  'Greedy trap': {
    C: 10,
    note: 'Greedy takes the densest item first and gets stuck. DP finds the optimum.',
    items: [
      { name: 'tiny-dense', w: 1, v: 3 },
      { name: 'big-a', w: 5, v: 10 },
      { name: 'big-b', w: 5, v: 10 },
    ],
  },
  'Flash crowd': {
    C: 12,
    note: 'A viral clip (forecast boosted by the burst detector) competes with steady content.',
    items: [
      { name: 'viral.mp4', w: 6, v: 40 },
      { name: 'logo.svg', w: 1, v: 6 },
      { name: 'news.html', w: 2, v: 11 },
      { name: 'ep1.mp4', w: 5, v: 21 },
      { name: 'ep2.mp4', w: 5, v: 19 },
      { name: 'cdn.css', w: 1, v: 5 },
    ],
  },
};

function solve(items: Item[], C: number) {
  const n = items.length;
  const dp: number[][] = Array.from({ length: n + 1 }, () => Array(C + 1).fill(0));
  const take: boolean[][] = Array.from({ length: n + 1 }, () => Array(C + 1).fill(false));
  for (let i = 1; i <= n; i++)
    for (let c = 0; c <= C; c++) {
      dp[i][c] = dp[i - 1][c];
      const it = items[i - 1];
      if (it.w <= c && dp[i - 1][c - it.w] + it.v > dp[i][c]) {
        dp[i][c] = dp[i - 1][c - it.w] + it.v;
        take[i][c] = true;
      }
    }
  const path = new Set<string>();
  const chosen: number[] = [];
  let c = C;
  for (let i = n; i >= 1; i--) {
    path.add(`${i},${c}`);
    if (take[i][c]) {
      chosen.push(i - 1);
      c -= items[i - 1].w;
    }
  }
  path.add(`0,${c}`);
  return { dp, take, path, chosen: chosen.reverse() };
}

function greedy(items: Item[], C: number) {
  const order = items.map((_, i) => i).sort((a, b) => items[b].v / items[b].w - items[a].v / items[a].w);
  let room = C;
  const chosen: number[] = [];
  for (const i of order) if (items[i].w <= room) { room -= items[i].w; chosen.push(i); }
  return chosen;
}

export function DPVisualizer() {
  const [preset, setPreset] = useState('CDN mini');
  const [items, setItems] = useState<Item[]>(PRESETS['CDN mini'].items);
  const [C, setC] = useState(PRESETS['CDN mini'].C);
  const [step, setStep] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(120);

  const valid = items.filter((it) => it.w >= 1 && it.v >= 0);
  const sol = useMemo(() => solve(valid, C), [valid, C]);
  const total = valid.length * (C + 1); // cells to fill (rows 1..n)
  const done = step >= total;
  const curI = Math.min(valid.length, Math.floor(step / (C + 1)) + 1);
  const curC = step % (C + 1);

  useEffect(() => {
    if (!playing) return;
    if (step >= total) { setPlaying(false); return; }
    const t = setTimeout(() => setStep((s) => s + 1), speed);
    return () => clearTimeout(t);
  }, [playing, step, total, speed]);

  const load = (name: string) => {
    setPreset(name);
    setItems(PRESETS[name].items);
    setC(PRESETS[name].C);
    setStep(0);
    setPlaying(false);
  };
  const filled = (i: number, c: number) => i === 0 || (i - 1) * (C + 1) + c < step;
  const g = greedy(valid, C);
  const gv = g.reduce((s, i) => s + valid[i].v, 0);
  const dv = sol.dp[valid.length]?.[C] ?? 0;
  const it = valid[curI - 1];

  return (
    <>
      <div className="page-intro">
        <h2>Watch the knapsack DP fill its table</h2>
        <p>
          <code>dp[i][c]</code> is the best benefit using the first <em>i</em> objects with <em>c</em> units of cache. Each cell looks at two
          cells in the row above: <em>skip</em> object i (<code>dp[i−1][c]</code>) or <em>keep</em> it (<code>dp[i−1][c−s<sub>i</sub>] + B<sub>i</sub></code>).
          When the table is full, the backtrack (green) reconstructs which objects to cache. Edit the objects, or try the presets.
        </p>
      </div>
      <div className="filters">
        <div className="seg">{Object.keys(PRESETS).map((p) => <button key={p} aria-pressed={preset === p} onClick={() => load(p)}>{p}</button>)}</div>
        <label className="field">Capacity C = {C}
          <input type="range" min={1} max={16} value={C} onChange={(e) => { setC(+e.target.value); setStep(0); setPlaying(false); }} />
        </label>
        <label className="field">Speed
          <input type="range" min={20} max={400} step={20} value={420 - speed} onChange={(e) => setSpeed(420 - +e.target.value)} />
        </label>
        <button className="btn btn-primary" onClick={() => { if (done) setStep(0); setPlaying((p) => !p); }}>{playing ? 'Pause' : done ? 'Replay' : 'Play'}</button>
        <button className="btn" onClick={() => setStep((s) => Math.min(total, s + 1))} disabled={done}>Step</button>
        <button className="btn" onClick={() => { setStep(total); setPlaying(false); }}>Finish</button>
        <button className="btn" onClick={() => { setStep(0); setPlaying(false); }}>Reset</button>
      </div>
      <div className="grid grid-2">
        <ChartCard className="span-2" title="DP table" sub={PRESETS[preset]?.note}>
          <div className="table-wrap">
            <table className="dp-table">
              <thead>
                <tr><th style={{ minWidth: 110, textAlign: 'left' }}>i \ c</th>{Array.from({ length: C + 1 }, (_, c) => <th key={c}>{c}</th>)}</tr>
              </thead>
              <tbody>
                {sol.dp.map((row, i) => (
                  <tr key={i}>
                    <th style={{ textAlign: 'left', paddingRight: 8, whiteSpace: 'nowrap' }}>
                      {i === 0 ? '0 (none)' : `${i} ${valid[i - 1].name} (s=${valid[i - 1].w}, B=${valid[i - 1].v})`}
                    </th>
                    {row.map((v, c) => {
                      const isCur = !done && i === curI && c === curC;
                      const isDep = !done && it && i === curI - 1 && (c === curC || (curC - it.w >= 0 && c === curC - it.w));
                      const cls = done && sol.path.has(`${i},${c}`) ? 'path' : isCur ? 'current' : isDep ? 'dep' : filled(i, c) ? 'filled' : '';
                      return <td key={c} className={cls}>{filled(i, c) || isCur ? v : ''}</td>;
                    })}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
          {!done && it && (
            <div className="note">
              Cell <code>dp[{curI}][{curC}]</code>: skip → <code>dp[{curI - 1}][{curC}] = {sol.dp[curI - 1][curC]}</code>
              {it.w <= curC ? (
                <>; keep → <code>dp[{curI - 1}][{curC - it.w}] + {it.v} = {sol.dp[curI - 1][curC - it.w] + it.v}</code>. Choose{' '}
                  <strong>{sol.take[curI][curC] ? 'keep' : 'skip'}</strong>.</>
              ) : <>; {it.name} (size {it.w}) does not fit in {curC}, so skip.</>}
            </div>
          )}
          {done && (
            <div className="note">
              <strong>Optimal cache: </strong>{sol.chosen.map((i) => valid[i].name).join(', ') || 'nothing'} with benefit <strong>{dv}</strong>.
              Greedy by benefit density picks {g.map((i) => valid[i].name).join(', ') || 'nothing'} = <strong>{gv}</strong>
              {gv < dv ? <> (<span className="down">{(((dv - gv) / dv) * 100).toFixed(0)}% worse</span>).</> : ' (optimal here too).'}
              {' '}Time Θ(n·C) = {valid.length}×{C + 1} = {valid.length * (C + 1)} cell updates.
            </div>
          )}
        </ChartCard>
        <ChartCard title="Objects" sub="Edit size (units) and benefit. Changes restart the animation.">
          {items.map((x, k) => (
            <div className="item-row" key={k}>
              <input type="text" value={x.name} aria-label="name" onChange={(e) => { const n = [...items]; n[k] = { ...x, name: e.target.value }; setItems(n); setStep(0); }} />
              <input type="number" min={1} max={16} value={x.w} aria-label="size" onChange={(e) => { const n = [...items]; n[k] = { ...x, w: Math.max(1, Math.min(16, +e.target.value || 1)) }; setItems(n); setStep(0); }} />
              <input type="number" min={0} max={999} value={x.v} aria-label="benefit" onChange={(e) => { const n = [...items]; n[k] = { ...x, v: Math.max(0, +e.target.value || 0) }; setItems(n); setStep(0); }} />
              <button className="icon-btn" aria-label="remove" onClick={() => { setItems(items.filter((_, j) => j !== k)); setStep(0); }}>×</button>
            </div>
          ))}
          {items.length < 9 && (
            <button className="btn" onClick={() => { setItems([...items, { name: `obj${items.length + 1}`, w: 2, v: 8 }]); setStep(0); }}>Add object</button>
          )}
        </ChartCard>
        <ChartCard title="Recurrence" sub="0/1 knapsack, as used by the cache planners">
          <div className="formula">{`dp[0][c] = 0
dp[i][c] = dp[i-1][c]                        if s_i > c
dp[i][c] = max(dp[i-1][c],
               dp[i-1][c - s_i] + B_i)        otherwise

B_i = f_i · (α·c_i/c̄ + β·l_i/l̄) − γ·[i ∉ cache]·α·c_i/c̄`}</div>
          <p style={{ marginTop: 10 }}>
            The last term is PACK-DP's churn penalty: an object not yet at the edge must be fetched, so it must earn its admission.
            In the simulator, the same recurrence runs with 1-D rolling arrays and a bit-packed keep table, or the O(n + C)-space Hirschberg variant.
          </p>
        </ChartCard>
      </div>
    </>
  );
}
