import { ReactNode, createContext, useContext, useMemo, useState } from 'react';
import {
  Bar, BarChart, CartesianGrid, Line, LineChart, ReferenceArea, ReferenceLine, ResponsiveContainer, Tooltip, XAxis, YAxis,
} from 'recharts';
import { CHROME, Mode } from '../data';

export const ModeContext = createContext<Mode>('light');
export const useMode = () => useContext(ModeContext);

// ------------------------------------------------------------------ basics --
export function Seg<T extends string | number>({ options, value, onChange, label }: {
  options: { value: T; label: string }[]; value: T; onChange: (v: T) => void; label?: string;
}) {
  return (
    <div className="field">
      {label && <span>{label}</span>}
      <div className="seg" role="group" aria-label={label}>
        {options.map((o) => (
          <button key={String(o.value)} aria-pressed={o.value === value} onClick={() => onChange(o.value)}>
            {o.label}
          </button>
        ))}
      </div>
    </div>
  );
}

export function Select<T extends string>({ options, value, onChange, label }: {
  options: { value: T; label: string }[]; value: T; onChange: (v: T) => void; label: string;
}) {
  return (
    <label className="field">
      <span>{label}</span>
      <select value={value} onChange={(e) => onChange(e.target.value as T)}>
        {options.map((o) => <option key={o.value} value={o.value}>{o.label}</option>)}
      </select>
    </label>
  );
}

export function Stat({ label, value, delta, deltaGood }: { label: string; value: string; delta?: ReactNode; deltaGood?: boolean | null }) {
  return (
    <div className="card stat">
      <div className="stat-label">{label}</div>
      <div className="stat-value">{value}</div>
      {delta !== undefined && (
        <div className="stat-delta">
          <span className={deltaGood === true ? 'up' : deltaGood === false ? 'down' : ''}>{delta}</span>
        </div>
      )}
    </div>
  );
}

/** Card with title, optional controls and a chart/table toggle (table view = accessibility relief). */
export function ChartCard({ title, sub, controls, table, children, className, note }: {
  title: string; sub?: ReactNode; controls?: ReactNode; table?: () => ReactNode; children: ReactNode; className?: string; note?: ReactNode;
}) {
  const [showTable, setShowTable] = useState(false);
  return (
    <section className={`card ${className ?? ''}`}>
      <div className="card-head">
        <div className="titles">
          <h3>{title}</h3>
          {sub && <div className="card-sub">{sub}</div>}
        </div>
        {controls}
        {table && (
          <button className="icon-btn" onClick={() => setShowTable((s) => !s)} aria-pressed={showTable}>
            {showTable ? 'Chart' : 'Table'}
          </button>
        )}
      </div>
      {showTable && table ? <div className="table-wrap">{table()}</div> : children}
      {note && <div className="note">{note}</div>}
    </section>
  );
}

// ------------------------------------------------------------------ legend --
export interface SeriesDef { id: string; name: string; color: string; dashed?: boolean; values: (number | null)[]; std?: (number | null)[] }

export function Legend({ series, hidden, onToggle, kind = 'line' }: {
  series: { id: string; name: string; color: string; dashed?: boolean }[]; hidden: Set<string>; onToggle: (id: string) => void; kind?: 'line' | 'rect';
}) {
  return (
    <div className="legend">
      {series.map((s) => (
        <button key={s.id} className="legend-item" aria-pressed={!hidden.has(s.id)} onClick={() => onToggle(s.id)} title="Click to show / hide">
          {kind === 'line' ? (
            <span className="key-line" style={{ borderTopColor: s.color, borderTopStyle: s.dashed ? 'dashed' : 'solid' }} />
          ) : (
            <span className="key-rect" style={{ background: s.color }} />
          )}
          {s.name}
        </button>
      ))}
    </div>
  );
}

export function useHidden(initial: string[] = []) {
  const [hidden, setHidden] = useState<Set<string>>(new Set(initial));
  const toggle = (id: string) =>
    setHidden((h) => {
      const n = new Set(h);
      if (n.has(id)) n.delete(id);
      else n.add(id);
      return n;
    });
  return { hidden, toggle, setHidden };
}

// ----------------------------------------------------------------- tooltip --
interface TipPayload { dataKey?: string | number; value?: number | null; color?: string; name?: string; payload?: Record<string, unknown> }
export function TipBox({ active, payload, label, fmt, title, series }: {
  active?: boolean; payload?: TipPayload[]; label?: string | number; fmt: (v: number) => string; title?: (l: string | number) => string;
  series: Map<string, { name: string; color: string; dashed?: boolean }>;
}) {
  if (!active || !payload?.length) return null;
  const rows = payload
    .filter((p) => p.value !== null && p.value !== undefined && series.has(String(p.dataKey)))
    .sort((a, b) => (b.value as number) - (a.value as number));
  return (
    <div className="tip">
      <div className="tip-title">{title ? title(label ?? '') : label}</div>
      {rows.map((p) => {
        const s = series.get(String(p.dataKey))!;
        return (
          <div className="tip-row" key={String(p.dataKey)}>
            <span className="key-line" style={{ borderTopColor: s.color, borderTopStyle: s.dashed ? 'dashed' : 'solid' }} />
            <span className="v">{fmt(p.value as number)}</span>
            <span className="n">{s.name}</span>
          </div>
        );
      })}
    </div>
  );
}

// -------------------------------------------------------------- line chart --
export function LineSeriesChart({ x, series, xLabel, yFmt, xFmt, height = 300, logX, logY, yDomain, areas, hiddenInit, tipTitle }: {
  x: number[]; series: SeriesDef[]; xLabel?: string; yFmt: (v: number) => string; xFmt?: (v: number) => string; height?: number;
  logX?: boolean; logY?: boolean; yDomain?: [number | string, number | string];
  areas?: { x1: number; x2: number; label: string }[]; hiddenInit?: string[]; tipTitle?: (l: string | number) => string;
}) {
  const mode = useMode();
  const c = CHROME[mode];
  const { hidden, toggle } = useHidden(hiddenInit);
  const rows = useMemo(
    () => x.map((xv, i) => {
      const r: Record<string, number | null> = { x: xv };
      for (const s of series) {
        const v = s.values[i];
        r[s.id] = v === null || v === undefined || !Number.isFinite(v) || (logY && v <= 0) ? null : v;
      }
      return r;
    }),
    [x, series, logY],
  );
  const sMap = new Map(series.filter((s) => !hidden.has(s.id)).map((s) => [s.id, s]));
  const tickFmt = xFmt ?? ((v: number) => String(v));
  return (
    <div>
      {series.length > 1 && <Legend series={series} hidden={hidden} onToggle={toggle} />}
      <ResponsiveContainer width="100%" height={height}>
        <LineChart data={rows} margin={{ top: 8, right: 16, bottom: xLabel ? 18 : 4, left: 4 }}>
          <CartesianGrid stroke={c.grid} vertical={false} />
          <XAxis
            dataKey="x" type="number" scale={logX ? 'log' : 'auto'} domain={['dataMin', 'dataMax']} ticks={x.length <= 12 ? x : undefined}
            tickFormatter={tickFmt} stroke={c.axis} tick={{ fill: c.muted, fontSize: 11.5 }} tickLine={false}
            allowDataOverflow
            label={xLabel ? { value: xLabel, position: 'insideBottom', offset: -10, fill: c.muted, fontSize: 12 } : undefined}
          />
          <YAxis
            scale={logY ? 'log' : 'auto'} domain={yDomain ?? (logY ? ['auto', 'auto'] : ['auto', 'auto'])} allowDataOverflow={!!logY}
            tickFormatter={yFmt} stroke={c.axis} tick={{ fill: c.muted, fontSize: 11.5 }} tickLine={false} axisLine={false} width={62}
          />
          {areas?.map((a, i) => (
            <ReferenceArea key={i} x1={a.x1} x2={a.x2} fill={c.muted} fillOpacity={0.1} ifOverflow="hidden"
              label={{ value: a.label, position: 'insideTop', fill: c.muted, fontSize: 11 }} />
          ))}
          <Tooltip
            cursor={{ stroke: c.axis, strokeWidth: 1 }}
            content={(p) => <TipBox {...(p as object)} fmt={yFmt} series={sMap} title={tipTitle ?? ((l) => `${xLabel ?? 'x'}: ${tickFmt(Number(l))}`)} />}
          />
          {series.filter((s) => !hidden.has(s.id)).map((s) => (
            <Line
              key={s.id} dataKey={s.id} name={s.name} stroke={s.color} strokeWidth={s.id === 'pack' ? 2.5 : 2}
              strokeDasharray={s.dashed ? '5 4' : undefined} dot={x.length <= 16 ? { r: 3.5, strokeWidth: 2, stroke: c.surface, fill: s.color } : false}
              activeDot={{ r: 4.5, strokeWidth: 2, stroke: c.surface }} isAnimationActive={false} connectNulls
            />
          ))}
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
}

// ---------------------------------------------------------- ranked bar chart --
export function RankedBars({ rows, fmt, height, domain, ticks }: {
  rows: { id: string; name: string; color: string; value: number; std?: number; dashed?: boolean }[]; fmt: (v: number) => string; height?: number;
  domain?: [number | string, number | string]; ticks?: number[];
}) {
  const mode = useMode();
  const c = CHROME[mode];
  const data = rows.map((r) => ({ ...r, label: r.std ? `${fmt(r.value)} ± ${fmt(r.std).replace('%', '')}` : fmt(r.value) }));
  const narrow = typeof window !== 'undefined' && window.innerWidth < 640;
  const sMap = new Map(rows.map((r) => [r.id, r]));
  return (
    <ResponsiveContainer width="100%" height={height ?? Math.max(160, rows.length * 34 + 30)}>
      <BarChart data={data} layout="vertical" margin={{ top: 4, right: narrow ? 92 : 104, bottom: 4, left: rows.some((r) => r.value < 0) ? 70 : 4 }} barCategoryGap={8}>
        <CartesianGrid stroke={c.grid} horizontal={false} />
        {rows.some((r) => r.value < 0) && <ReferenceLine x={0} stroke={c.axis} />}
        <XAxis type="number" domain={domain ?? [0, 'auto']} ticks={ticks} tickFormatter={fmt} stroke={c.axis} tick={{ fill: c.muted, fontSize: 11.5 }} tickLine={false} />
        <YAxis type="category" dataKey="name" width={narrow ? 104 : 150} stroke={c.axis} tick={{ fill: c.ink2, fontSize: narrow ? 11.5 : 12.5 }} tickLine={false} axisLine={false} />
        <Tooltip
          cursor={{ fill: c.grid, fillOpacity: 0.5 }}
          content={({ active, payload }) => {
            if (!active || !payload?.length) return null;
            const r = payload[0].payload as (typeof data)[number];
            const s = sMap.get(r.id)!;
            return (
              <div className="tip">
                <div className="tip-row">
                  <span className="key-rect" style={{ background: s.color }} />
                  <span className="v">{fmt(r.value)}</span>
                  <span className="n">{r.name}</span>
                </div>
                {r.std ? <div className="tip-title" style={{ marginTop: 4 }}>± {fmt(r.std)} (1 s.d. across seeds)</div> : null}
              </div>
            );
          }}
        />
        <Bar
          dataKey="value" barSize={20} isAnimationActive={false}
          label={(lp: unknown) => {
            // value label at the data end of the bar: right of positive bars, left of negative ones
            const p = lp as { x: number; y: number; width: number; height: number; index: number };
            const row = data[p.index];
            if (!row) return <g />;
            const left = Math.min(p.x, p.x + p.width), right = Math.max(p.x, p.x + p.width);
            const neg = row.value < 0;
            return (
              <text x={neg ? left - 6 : right + 6} y={p.y + p.height / 2} dy="0.35em" textAnchor={neg ? 'end' : 'start'} fill={c.ink2} fontSize={12}>
                {row.label}
              </text>
            );
          }}
          shape={(props: unknown) => {
            const p = props as { x: number; y: number; width: number; height: number; payload: (typeof data)[number] };
            // negative values: normalise a negative width and round only the data end
            const neg = p.payload.value < 0;
            const w = Math.abs(p.width);
            const x0 = p.width < 0 ? p.x + p.width : p.x;
            const r = Math.min(4, w / 2, p.height / 2);
            const d = neg
              ? `M${x0 + w},${p.y} h-${w - r} a${r},${r} 0 0 0 -${r},${r} v${p.height - 2 * r} a${r},${r} 0 0 0 ${r},${r} h${w - r} z`
              : `M${x0},${p.y} h${w - r} a${r},${r} 0 0 1 ${r},${r} v${p.height - 2 * r} a${r},${r} 0 0 1 -${r},${r} h-${w - r} z`;
            return (
              <path d={d} fill={p.payload.color} fillOpacity={p.payload.dashed ? 0.45 : 1}
                stroke={p.payload.dashed ? p.payload.color : 'none'} strokeDasharray={p.payload.dashed ? '4 3' : undefined} />
            );
          }}
        >
        </Bar>
      </BarChart>
    </ResponsiveContainer>
  );
}

export function Loading({ error }: { error?: string | null }) {
  return (
    <div className="empty">
      {error ? (
        <>Could not load results ({error}).<br />Run <code>./core/build/cdnopt experiment all</code> then <code>npm run sync-data</code>.</>
      ) : 'Loading…'}
    </div>
  );
}
