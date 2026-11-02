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

const TABS = [
  { id: 'overview', label: 'Overview', el: <Overview /> },
  { id: 'timeline', label: 'Timeline', el: <Timeline /> },
  { id: 'sweeps', label: 'Sweeps', el: <Sweeps /> },
  { id: 'ablation', label: 'Ablation', el: <Ablation /> },
  { id: 'daa', label: 'DAA lab', el: <Algorithms /> },
  { id: 'abr', label: 'Multi-bitrate (MCKP)', el: <Abr /> },
  { id: 'dp', label: 'DP visualizer', el: <DPVisualizer /> },
  { id: 'live', label: 'Live lab', el: <LiveLab /> },
];

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
    return TABS.some((t) => t.id === h) ? h : 'overview';
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
              <div className="brand-sub">Knapsack DP vs. online caching · Design &amp; Analysis of Algorithms</div>
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
            <button key={t.id} role="tab" className="tab" aria-selected={tab === t.id} onClick={() => go(t.id)}>{t.label}</button>
          ))}
        </nav>
      </header>
      <main>{TABS.find((t) => t.id === tab)?.el}</main>
      <div className="footer">
        All results are produced by the C++ engine in <code>core/</code> (<code>cdnopt experiment all</code>). * = oracle with future knowledge (reference bound).
      </div>
    </ModeContext.Provider>
  );
}
