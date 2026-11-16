// How SHADE works: the derivation in four steps, with a pipeline diagram.
export function Method() {
  return (
    <div className="page">
      <div className="page-head">
        <h2>How SHADE works</h2>
        <p>
          SHADE (Shadow-price Admission and hit-Density Eviction) turns the offline knapsack optimum for caching into an online rule.
          It uses one learned quantity for two decisions: what to evict, and whether a new object deserves to be cached at all.
        </p>
      </div>

      <section className="card">
        <div className="card-head"><div className="titles"><h3>The pipeline</h3><div className="card-sub">Every request updates the statistics; every 2<sup>18</sup> requests the index table is refitted.</div></div></div>
        <div className="pipeline" role="img" aria-label="Request flows into reuse statistics, which produce the index table, which drives eviction and admission">
          <div className="pipe-step"><span className="pipe-n">1</span><b>Observe</b><span>Every request, cached or not, records how long the object waited since its last request.</span></div>
          <div className="pipe-arrow" aria-hidden>→</div>
          <div className="pipe-step"><span className="pipe-n">2</span><b>Estimate</b><span>Kaplan–Meier reuse curves per class (request count × last gap), with shrinkage.</span></div>
          <div className="pipe-arrow" aria-hidden>→</div>
          <div className="pipe-step"><span className="pipe-n">3</span><b>Index</b><span>Best hits per byte per unit time over any holding time: the online knapsack density.</span></div>
          <div className="pipe-arrow" aria-hidden>→</div>
          <div className="pipe-step accent"><span className="pipe-n">4</span><b>Decide</b><span>Evict the lowest index. Admit only if the newcomer beats what it displaces.</span></div>
        </div>
      </section>

      <div className="grid-2">
        <section className="card prose">
          <h3>1 · Caching is a knapsack</h3>
          <p>
            Offline, each pair of consecutive requests to an object is an item. Keeping the object across that gap earns one hit and costs
            <code>size × gap</code> of cache space-time. The run offers <code>capacity × requests</code>. Choosing intervals is a knapsack;
            its fractional relaxation, solved by the greedy rule, is the <b>PFOO-L</b> bound we plot as the floor no policy can beat.
          </p>
          <p>LP duality: the optimum keeps an interval iff its hits per space-time exceed one <b>shadow price λ</b>.</p>
        </section>
        <section className="card prose">
          <h3>2 · Learn what the future looks like</h3>
          <p>
            Online we don’t know the gap. SHADE learns its distribution from the last 2<sup>23</sup> requests, for objects in and out of the cache,
            grouped by request count and the previous gap.
          </p>
          <p>
            Objects still waiting are <b>right-censored</b> (Kaplan–Meier), old data fades with a half-life of 2<sup>22</sup> requests,
            and sparse classes borrow strength from their parent (empirical Bayes).
          </p>
        </section>
        <section className="card prose">
          <h3>3 · Value an object: the index</h3>
          <p>
            For an object that has waited <i>a</i> requests:
            <code className="block">I = max over τ of P(reuse before τ) ÷ E[residency until τ]</code>
            and <code>rank = I ÷ size</code> is hits per byte per request. Taking the max over every holding time τ is an optimal-stopping index:
            no horizon to tune, and it sees the second, daily reuse peak that real Wikipedia traffic has.
          </p>
        </section>
        <section className="card prose">
          <h3>4 · Decide: evict and admit with one number</h3>
          <p>
            <b>Evict</b> the lowest-ranked of 32 sampled objects plus a pool of earlier candidates. Its rank is the current shadow price.
          </p>
          <p>
            <b>Admit</b> by the knapsack exchange argument: store the new object only if it is worth at least twice every object it would displace.
            On CDN traffic, where more than half of all objects are requested exactly once, this keeps the cache from filling with them.
          </p>
        </section>
      </div>

      <section className="card prose">
        <h3>What is new, and what is borrowed</h3>
        <ul>
          <li><b>Borrowed:</b> the knapsack/PFOO view (Berger et al. 2018), hit density as a rank (LHD, 2018), sampled eviction (Redis), Kaplan–Meier and empirical Bayes.</li>
          <li><b>This project:</b> one learned density drives both eviction and admission through the exchange test; the statistics cover uncached objects, so admission can value them; the optimal-stopping index replaces a fixed horizon.</li>
          <li><b>Not done:</b> comparison with learned policies (LRB), production deployment, traces from other operators.</li>
        </ul>
      </section>
    </div>
  );
}
