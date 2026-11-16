import { useEffect, useState } from 'react';
import { useResolvedMode } from './data';
import { ModeContext } from './components/ui';
import { Overview } from './components/Overview';
import { Timeline } from './components/Timeline';
import { Sweeps } from './components/Sweeps';
import { Ablation } from './components/Ablation';
import { Algorithms } from './components/Algorithms';
import { Abr } from './components/Abr';
import { DPVisualizer } from './components/DPVisualizer';
import { LiveLab } from './components/LiveLab';
import { RealTraces } from './components/RealTraces';
import { Method } from './components/Method';

// Earlier synthetic-workload study, kept as one section with its own sub-navigation.
const V1_TABS = [
  { id: 'v1-overview', label: 'Overview', el: <Overview /> },
  { id: 'v1-timeline', label: 'Timeline', el: <Timeline /> },
  { id: 'v1-sweeps', label: 'Sweeps', el: <Sweeps /> },
  { id: 'v1-ablation', label: 'Ablation', el: <Ablation /> },
  { id: 'v1-abr', label: 'Multi-bitrate', el: <Abr /> },
  { id: 'v1-live', label: 'Live lab', el: <LiveLab /> },
];

function SimulationV1({ sub, go }: { sub: string; go: (id: string) => void }) {
  const cur = V1_TABS.find((t) => t.id === sub) ?? V1_TABS[0];
  return (
    <div>
      <div className="v1-banner">
        Earlier stage of the project: the PACK-DP planner on <b>synthetic</b> traffic. On real traces it did not hold up, which led to SHADE.
      </div>
      <div className="subnav">
        <span className="label">Section</span>
        <div className="seg" role="group" aria-label="Simulation study section">
          {V1_TABS.map((t) => (
            <button key={t.id} aria-pressed={cur.id === t.id} onClick={() => go(t.id)}>{t.label}</button>
          ))}
        </div>
      </div>
      {cur.el}
    </div>
  );
}

const TABS = [
  { id: 'results', label: 'Real-trace results' },
  { id: 'method', label: 'How SHADE works' },
  { id: 'daa', label: 'Knapsack solvers' },
  { id: 'dp', label: 'DP visualizer' },
  { id: 'v1', label: 'Simulation study (v1)' },
];
const tabOf = (id: string) => (id.startsWith('v1') ? 'v1' : id);

function readStored(key: string, fallback: string) {
  try {
    return localStorage.getItem(key) ?? fallback;
  } catch {
    return fallback;
  }
}
function store(key: string, v: string) {
  try {
    localStorage.setItem(key, v);
  } catch {
    /* storage unavailable: ignore */
  }
}

export default function App() {
  const initialTab = () => {
    const h = window.location.hash.slice(1);
    return TABS.some((t) => t.id === tabOf(h)) ? h : 'results';
  };
  const [tab, setTab] = useState(initialTab);
  const [pref, setPref] = useState<'auto' | 'light' | 'dark'>(() => readStored('cdn-theme', 'auto') as 'auto' | 'light' | 'dark');
  const mode = useResolvedMode(pref);

  useEffect(() => {
    if (pref === 'auto') document.documentElement.removeAttribute('data-theme');
    else document.documentElement.setAttribute('data-theme', pref);
    store('cdn-theme', pref);
  }, [pref]);
  useEffect(() => {
    const on = () => setTab(initialTab());
    window.addEventListener('hashchange', on);
    return () => window.removeEventListener('hashchange', on);
  }, []);
  const go = (id: string) => {
    setTab(id);
    history.replaceState(null, '', `#${id}`);
    window.scrollTo({ top: 0 });
  };

  return (
    <ModeContext.Provider value={mode}>
      <header className="app-header">
        <div className="header-row">
          <div className="brand">
            <div className="brand-mark" aria-hidden>
              <svg width="16" height="16" viewBox="0 0 16 16"><rect x="1" y="8" width="3.5" height="7" rx="1" fill="#fff" /><rect x="6.25" y="4.5" width="3.5" height="10.5" rx="1" fill="#fff" /><rect x="11.5" y="1" width="3.5" height="14" rx="1" fill="#fff" /></svg>
            </div>
            <div>
              <h1>CDN Cache Optimizer</h1>
              <div className="brand-sub">SHADE: knapsack-dual caching on real CDN traces · Design &amp; Analysis of Algorithms</div>
            </div>
          </div>
          <div className="seg" role="group" aria-label="Theme">
            {(['auto', 'light', 'dark'] as const).map((p) => (
              <button key={p} aria-pressed={pref === p} onClick={() => setPref(p)}>{p}</button>
            ))}
          </div>
        </div>
        <nav className="tabs" role="tablist">
          {TABS.map((t) => (
            <button key={t.id} role="tab" className="tab" aria-selected={tabOf(tab) === t.id} onClick={() => go(t.id)}>{t.label}</button>
          ))}
        </nav>
      </header>
      <main>
        {tabOf(tab) === 'results' && <RealTraces />}
        {tabOf(tab) === 'method' && <Method />}
        {tabOf(tab) === 'daa' && <Algorithms />}
        {tabOf(tab) === 'dp' && <DPVisualizer />}
        {tabOf(tab) === 'v1' && <SimulationV1 sub={tab} go={go} />}
      </main>
      <div className="footer">
        Real-trace results: <code>lab/</code> (<code>lab/experiments/run_heldout.sh</code>). Knapsack solvers and the v1 study: <code>core/</code>.
        * = offline reference with future knowledge.
      </div>
    </ModeContext.Provider>
  );
}
