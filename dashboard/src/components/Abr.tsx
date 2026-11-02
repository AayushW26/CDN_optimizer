import { useState } from 'react';
import { AbrFile, slotColor, useJson } from '../data';
import { ChartCard, LineSeriesChart, Loading, Seg, useMode } from './ui';

const METRICS = [
  { key: 'benefitRatio', label: 'Benefit captured' },
  { key: 'hitRatio', label: 'Request hit ratio' },
  { key: 'byteHitRatio', label: 'Byte hit ratio' },
  { key: 'costSavedPct', label: 'Origin $ saved' },
  { key: 'transcodePct', label: 'Hits served by transcoding' },
];

export function Abr() {
  const { data, error } = useJson<AbrFile>('abr.json');
  const [metric, setMetric] = useState('benefitRatio');
  const mode = useMode();
  if (!data) return <Loading error={error} />;
  const pctOfOne = metric === 'benefitRatio' || metric === 'hitRatio' || metric === 'byteHitRatio';
  // policy identity colours: own mapping inside this chart (different entities from the algorithm set)
  const colorIdx = [1, 5, 6, 0];
  const series = data.policies.map((p, i) => ({
    id: p.name, name: p.name, color: slotColor(colorIdx[i] ?? i, mode),
    values: (p[metric] ?? []).map((v: number) => (pctOfOne ? v * 100 : v)),
  }));
  const yFmt = (v: number) => `${v.toFixed(0)}%`;
  return (
    <>
      <div className="page-intro">
        <h2>Multi-bitrate video: a Multiple-Choice Knapsack</h2>
        <p>
          Adaptive-bitrate video stores each title in several renditions ({data.ladder.map((l) => l.name).join(', ')}).
          An edge can transcode <em>down</em> from a cached higher rendition (keeping {Math.round(data.transcodeKeep * 100)}% of the benefit), but not up.
          Treating each rendition as an independent 0/1 item double-counts. The right model is the
          <strong> Multiple-Choice Knapsack Problem</strong>: per video pick <em>at most one</em> rendition j with value
          Σ<sub>q≤j</sub> f<sub>q</sub>·B<sub>q</sub>·(transcode factor). The DP runs in O(C·Σ|group|).
        </p>
      </div>
      <div className="filters">
        <Seg label="Metric" options={METRICS.map((m) => ({ value: m.key, label: m.label }))} value={metric} onChange={setMetric} />
      </div>
      <div className="grid grid-2">
        <ChartCard
          className="span-2"
          title={`${METRICS.find((m) => m.key === metric)!.label} vs cache size`}
          sub={`${data.videos} videos × ${data.ladder.length} renditions, Zipf popularity, per-title device mix. Plans use the previous epoch's counts. Mean of ${data.seeds} seeds. Largest MCKP solve: ${data.mckpMaxMs.toFixed(0)} ms, ${(data.mckpMemKB / 1024).toFixed(1)} MB.`}
          table={() => (
            <table>
              <thead><tr><th>Policy</th>{data.x.map((x) => <th key={x}>{(x * 100).toFixed(0)}%</th>)}</tr></thead>
              <tbody>{series.map((s) => <tr key={s.id} className={s.id.startsWith('MCKP') ? 'hl' : ''}><td>{s.name}</td>{s.values.map((v, i) => <td key={i}>{yFmt(v)}</td>)}</tr>)}</tbody>
            </table>
          )}
        >
          <LineSeriesChart x={data.x} series={series} xLabel={data.xLabel} logX xFmt={(v) => `${(v * 100).toFixed(0)}%`} yFmt={yFmt} height={330} />
        </ChartCard>
        <ChartCard title="Rendition ladder" sub="Bitrate and device-mix share of requests.">
          <div className="table-wrap"><table>
            <thead><tr><th>Rendition</th><th>Bitrate</th><th>Request share</th></tr></thead>
            <tbody>{data.ladder.map((l) => <tr key={l.name}><td>{l.name}</td><td>{l.kbps >= 1000 ? `${l.kbps / 1000} Mb/s` : `${l.kbps} kb/s`}</td><td>{(l.share * 100).toFixed(0)}%</td></tr>)}</tbody>
          </table></div>
        </ChartCard>
        <ChartCard title="The four policies">
          <ul className="kbd-list">
            <li><strong>LRU (per version)</strong>: online; a request is a hit if the same or a higher rendition is cached.</li>
            <li><strong>Independent 0/1 KP</strong>: one item per (video, rendition). Ignores transcoding, so it often stores two renditions of the same title.</li>
            <li><strong>Top-version KP</strong>: one item per video, the highest requested rendition (serves everything, but is large).</li>
            <li><strong>MCKP</strong>: exact group DP, choosing the best single rendition per title (or none).</li>
          </ul>
        </ChartCard>
      </div>
    </>
  );
}
