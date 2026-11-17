# Presentation prep guide: CDN_optimizer / SHADE

Written for someone who has not worked on the project. Read sections 1–3 first (15 min), then
skim 4–7. Every number here comes from the repo's own docs/results, checked against the tables
in `docs/LAB_RESULTS.md`. Numbers marked *(my calc)* were computed from those tables.

---

## 1. The 60-second version

> A CDN edge server has a small disk and millions of objects it *could* store. Every time a
> request misses, it pays a trip to the origin. The question is: **which objects should it keep,
> and which new objects are not even worth storing?**
>
> We treat that as a **knapsack problem** (pick the most valuable items that fit in limited
> space). From the knapsack math we derived an online caching policy called **SHADE**. We tested
> it on **90 million real requests** from Wikipedia/Wikimedia against **8 published policies**.
> We froze the method *before* touching the test data. SHADE has the fewest misses in **12 of 15**
> test settings, and cuts misses by **6–8%** vs the best baseline at small and medium cache sizes.
> It **loses at large caches (10%)**, and we say so openly.

If you remember only one sentence: **"Caching is a knapsack; SHADE is the online version of the knapsack's optimal answer, and it is admission-aware, which matters because most CDN objects are requested once."**

---

## 2. Concepts from zero (a glossary you can talk from)

| Term | Plain meaning |
|---|---|
| **CDN / edge server** | Servers close to users that keep copies of popular content so the far-away "origin" isn't hit every time. |
| **Cache hit / miss** | Hit = object already stored at the edge. Miss = must fetch from origin (slow, costs money). |
| **Object miss ratio** | Fraction of *requests* that miss. **Lower is better.** The headline metric. |
| **Byte miss ratio** | Fraction of *bytes* that miss. Cares about big files more. Secondary metric. |
| **Eviction** | When the cache is full, deciding which stored object to throw out. (LRU = throw out the least recently used.) |
| **Admission** | When a miss happens, deciding whether to store the new object *at all*. Most research focuses on eviction; this project argues admission matters just as much. |
| **One-hit wonder** | An object requested exactly once, ever. **53–68%** of objects in these traces. Storing them wastes space. |
| **Trace** | A recorded log of real requests (object id, size, time). We replay it through each policy in a simulator. |
| **Cache size as % of footprint** | Footprint = total bytes of all distinct objects. A "1% cache" holds 1% of that. Real edges are usually small relative to the catalogue. |
| **Held-out / test set** | Data the method designers never looked at while building the method. The honest evaluation. |
| **Development slice** | The data used for design and tuning (Wikipedia 2018, requests 0–10M). |
| **Knapsack problem** | Items with weight and value, bag with a weight limit: pick items for max total value. NP-hard in general, but greedy-by-value-per-weight is optimal for the *fractional* version. |
| **LP relaxation / duality / shadow price** | Relax "all or nothing" to "fractions allowed" → solvable by greedy. The dual variable is a single price per unit of capacity; the optimal answer keeps exactly the items whose value-per-weight beats that price. |
| **Upper bound (PFOO-L)** | A bound from the fractional relaxation: *no* policy, even one that sees the future, can do better. Used as the ceiling in the charts. |

**Baselines (the 8 competitors) in one line each**

- **LRU**: evict least recently used. The classic default.
- **FIFO**: evict oldest inserted.
- **SIEVE** (NSDI'24), **S3-FIFO** (SOSP'23): modern FIFO-based designs that filter out one-hit wonders cheaply.
- **GDSF**: scores each object by (frequency × cost) / size. Size-aware; the strongest baseline here.
- **W-TinyLFU**: small admission window plus frequency sketch (used in Caffeine).
- **AdaptSize** (NSDI'17): probabilistic admission that favors small objects.
- **LHD** (NSDI'18): evicts the object with the lowest estimated hit density. Closest in spirit to SHADE.

---

## 3. The story (this is your narrative arc)

1. **v1 (the original DAA project, `core/`)**: model caching as a knapsack *per time epoch*. Solve it
   exactly with dynamic programming on a forecast of popularity (**PACK-DP**). Implemented 9
   knapsack solvers. On **synthetic** traffic it beat every baseline, 150 of 150 paired tests.
2. **Reality check**: on **real** Wikipedia traffic PACK-DP came **4th of 6**. Why? Real traffic is
   dominated by objects requested once, so prefetching planned contents wasted bandwidth. Real data
   also exposed a stack-overflow bug in the recursive branch-and-bound solver (now iterative).
3. **Two lessons**: (a) learn from the *real request stream*, not from a forecast; (b) **admission
   matters as much as eviction**.
4. **v2 (the cache lab, `lab/`)**: derive an *online* policy from the same knapsack theory. That is **SHADE**.
5. **Honest evaluation**: separate dev and test data, freeze the method in git before testing,
   8 strong baselines, a theoretical bound, 460 automated checks.

This "it worked on synthetic data, failed on real data, so we rebuilt" arc is a strength. Tell it plainly.

---

## 4. How SHADE works (explain it in this order)

**Step 1: Caching offline is a knapsack.** Every time an object is requested again, the gap between
two consecutive requests is a *reuse interval*. Keeping the object across it:
- earns **1 hit**,
- costs **size × gap** of cache space-time.

Choosing which intervals to cover is a knapsack: value = 1, weight = size × gap, capacity = cache
size × trace length. Solve the fractional version with the greedy rule from class → that is the
PFOO-L bound. LP duality says the optimum is a **threshold rule**: keep an interval iff
`1/(size × gap)` beats one **shadow price λ**.

**Step 2: Online, we don't know the gap.** So SHADE *learns* it:
- Tracks every object seen recently (cached or not), puts it in a class by (request count, last gap).
- Builds reuse-time histograms per class.
- **Kaplan–Meier censoring correction**: objects still waiting for their next request are not "never
  reused", they're unfinished. Ignoring this biases toward short gaps.
- **Exponential forgetting** (half-life 2²² requests) so it adapts to drift.
- **Empirical-Bayes shrinkage**: sparse classes borrow statistics from broader ones.

**Step 3: Rank = hits per byte per unit time.** For each object, an *optimal-stopping index*: the
best ratio of (hit probability) / (expected space-time) over any holding time τ. Divide by size.

**Step 4: Evict** the lowest-ranked among 32 sampled objects plus a carry-over pool (like Redis).
The victim's rank is the live estimate of the shadow price λ.

**Step 5: Admit only if it wins the knapsack exchange test**: a new object must be worth **≥ 2×** what
it would displace (`admitSlack = 2.0`, `lab/src/shade.hpp:39`, the one tuned constant, chosen on the dev slice only).

**What is new vs borrowed** (be upfront; `docs/METHOD.md` §4): the knapsack view comes from PFOO, hit
density from LHD, sampled eviction from Redis, the statistics are textbook. **New:** (1) one learned
density used for both eviction *and* admission via the exchange test; (2) reuse statistics over *all*
objects, so uncached objects can be valued before admission; (3) the optimal-stopping index. The docs
say they have not done an exhaustive literature search; repeat that if asked.

---

## 5. Suggested slide outline (≈10 minutes; stretch/shrink as needed)

| # | Slide | Time | What to say | Visual |
|---|---|---|---|---|
| 1 | **Title + question** | 0:30 | "Which objects should a CDN edge keep, and which are not worth storing?" | none |
| 2 | **Why it matters** | 1:00 | Misses cost latency and origin egress dollars. Cache is tiny vs catalogue. 53–68% of objects are one-hit wonders. | trace table (README) |
| 3 | **Caching = knapsack** | 1:30 | Reuse interval: 1 hit for size × gap space-time. Fractional knapsack → PFOO-L bound; duality → threshold. | hand-drawn: bag + items |
| 4 | **v1 → lesson** | 1:00 | PACK-DP won 150/150 on synthetic, 4th of 6 on real. Learn from real streams; admission matters. | `docs/figures/compare.png` (v1) |
| 5 | **SHADE in one picture** | 1:30 | Learn reuse distributions → rank by hits/byte/time → evict lowest → admit only if 2× better. | flow diagram (make this one) |
| 6 | **Evaluation protocol** | 1:00 | Dev vs held-out, frozen in git, 8 baselines, upper bound, 460 checks. | 3 trace table + commit name |
| 7 | **Main result** | 1:30 | 12/15 wins; 6–8% fewer misses than GDSF at small/medium caches; wins everywhere on text. | `docs/figures/lab_miss_ratio.png` |
| 8 | **Where it loses** | 1:00 | At 10% (and 3% on Wikipedia 2018) GDSF wins by up to 24%. Visible on the dev slice already; reported, not retuned. | `docs/figures/lab_improvement.png` |
| 9 | **Why it works (ablation)** | 0:45 | No features (one class): +5–29% misses at 0.1–1% caches. No admission: +4.5–8.6% (at small caches). No censoring correction: up to +12.6%. | `docs/figures/lab_ablation.png` |
| 10 | **Limitations + takeaways** | 0:45 | Simulation only, one operator, no learned baselines (LRB), more metadata than LRU. | bullets |

Optional DAA slide (if your audience is the course instructor): the 9 knapsack solvers, Hirschberg
O(n+C) space (159 KB vs 512 MB), FPTAS, MCKP for multi-bitrate video. Use the dashboard's **Knapsack
solvers** and **DP visualizer** tabs. Detail is in `docs/REPORT.md` §3 and §7.5.

---

## 6. Cheat sheet of numbers

**Data (held-out unless noted)**

| Trace | Requests | Objects | Footprint | Requested once |
|---|---:|---:|---:|---:|
| Wikipedia 2018 | 40M | 4.5M | 503 GB | 55% |
| Wikimedia 2019 upload (media) | 40M | 6.9M | 610 GB | 53% |
| Wikimedia 2019 text (HTML) | 10.2M | 3.7M | 89 GB | 68% |
| Dev slice: Wikipedia 2018, requests 0–10M | 10M | 2.0M | 191 GB | 61.5% |

**SHADE vs best baseline (object miss ratio, + = SHADE has fewer misses)**

| Trace (best baseline) | 0.1% | 0.3% | 1% | 3% | 10% |
|---|---:|---:|---:|---:|---:|
| Wikipedia 2018 (GDSF) | +6.8% | +7.9% | +6.2% | −2.9% | −23.8% |
| Upload 2019 (GDSF) | +7.1% | +7.3% | +6.7% | +1.1% | −17.7% |
| Text 2019 (S3-FIFO) | +1.1% | +2.1% | +3.6% | +4.0% | +3.6% |

**Headline claims (all verified against the tables)**
- Wins in **12 of 15** held-out settings (3 + 4 + 5 by trace).
- vs LRU: removes **5–52%** of misses, mean **30%** *(my calc, matches README)*.
- Closeness to the bound at 1% cache *(my calc)*: SHADE closes about **80%** of the LRU→PFOO-L gap on Wikipedia 2018, **75%** on upload, **39%** on text.
- Example absolute numbers, Wikipedia 2018 at 1% cache: LRU 0.472, GDSF 0.251, **SHADE 0.235**, PFOO-L bound 0.174.
- Byte objective **SHADE-B**: beats best baseline (S3-FIFO) in 10 of 15 settings, but only by 0.3–1.9%. Call it "modest".
- Tests: 460 checks, 0 failures (I ran `make lab-test` and confirmed). They include "no policy exceeds
  capacity", "no hit on a first request", and "no policy, offline ones included, beats the PFOO-L bound".
- v1: PACK-DP 150/150 paired wins on synthetic data; 4th of 6 on real.

---

## 7. Likely questions and good answers

**Q: Why does SHADE lose to GDSF at large caches?**
A: When the cache is big, almost everything worth keeping fits; the problem becomes ranking by simple
frequency/size, which GDSF does cheaply and without learning noise. SHADE's learned estimates have
more variance, and its 2× admission margin is conservative. This showed up on the development slice
before testing, and we reported it instead of retuning on test data. (Be careful: the docs state the
fact, not a proven mechanism. Say "our hypothesis".)

**Q: Is the 6–8% improvement big?**
A: It's a *relative* reduction in misses (e.g., 0.431 → 0.402 on Wikipedia 2018 at 0.1%, about 3 points
absolute). For CDNs, a few percent of origin traffic is meaningful, and it's against the *best* of 8
baselines. Also note the real ceiling: PFOO-L is 0.331 there, so the achievable room is about 10 points.

**Q: How do you know you didn't overfit to the test traces?**
A: One tuned constant (exchange margin 2), tuned on a separate dev slice. The method was committed
(`3869fa7 "Add cache lab and SHADE (frozen before held-out evaluation)"`) *before* any held-out run, and
nothing changed afterward. Ablation "exchange margin 1 (untuned)" is only 0.1–0.7% worse, so the result
isn't sensitive to it.

**Q: Why not compare with learned policies like LRB?**
A: Out of scope and stated as a limitation. LRB needs ML training infrastructure; SHADE is a
statistical policy with no model training. Don't claim SHADE beats learned caches.

**Q: Is it practical to deploy?**
A: Not shown. It tracks reuse stats for every object seen in the last 2²³ requests: ~12 bytes per
object plus a history queue (~64 MB), more than LRU/S3-FIFO. Simulation speed is 0.3–2 µs per request.
A ghost table could bound memory; that is not evaluated.

**Q: What is "logical time"?**
A: Time = request count, not seconds. The LHD and PFOO papers do the same. Real deployments see bursts
in wall-clock time.

**Q: Why does admission matter so much?**
A: 53–68% of objects are requested once. Every one-hit wonder that you store evicts something useful.
Ablation: turning admission off raises misses by 4.5–8.6% at small caches on the two media traces. At
10% caches, and on the text trace, it makes no difference (the ablation table shows ~0%).

**Q: Where does the knapsack/DAA content show up in the final policy?**
A: The theory: fractional knapsack greedy → PFOO-L bound; duality → threshold/shadow price; greedy
exchange argument → admission test. The v1 code also contains the full solver toolkit (DP, Hirschberg,
branch and bound, meet-in-the-middle, FPTAS, MCKP), which is still tested (`make build test`).

**Q: Why is exact knapsack not used online?**
A: Per-epoch exact DP needs a forecast, and forecast error (4–8 points of benefit in v1) dominated
solver error. On real traces the planner also wasted bandwidth prefetching. So we moved from "plan then
execute" to "learn and decide per request".

**Q: Is knapsack NP-hard? How did you solve it?**
A: Yes (reduction from subset-sum). DP is pseudo-polynomial Θ(nC); the Hirschberg-style version keeps
Θ(nC) time but needs only Θ(n+C) space; FPTAS gives (1−ε)·OPT in O(n²/ε); greedy has an OPT − greedy ≤
v_break bound. See `docs/REPORT.md` §3.

---

## 8. Honest weaknesses (say them before someone else does)

1. Loses at 10% cache size, and at 3% on Wikipedia 2018.
2. Simulation only; all three traces are from one operator (Wikimedia).
3. No learned baselines (LRB, GL-Cache).
4. More metadata than LRU / S3-FIFO.
5. SHADE-B (byte objective) gains are small (0.3–1.9%) and it loses at the smallest caches.
6. Novelty claim is hedged in the docs ("we are not aware of a prior policy...", no exhaustive literature search).

---

## 9. Pitfalls when presenting

- **Sign convention differs between files.** `README.md`'s table prints SHADE's change as *negative
  when SHADE is better* (−6.8%). `docs/LAB_RESULTS.md` and `docs/figures/lab_improvement.png` use *positive when
  SHADE is better* (+6.8%). Pick one and say what it means on the slide.
- **Relative vs absolute.** "6.8% fewer misses" is relative, not 6.8 percentage points.
- **Lower is better** for every miss-ratio chart; the lowest curve wins.
- **Belady-Size\*** and **PFOO-L** are offline references that see the future. Never call them competitors.
- **Dev slice results are not evidence.** Quote only the three held-out traces for claims.
- **Cache size is a % of the trace footprint**, not of the number of objects.

---

## 10. Live demo (2 minutes, optional)

I confirmed the dashboard builds. To run it:

```bash
make dashboard && make serve      # then open http://127.0.0.1:8787
```

Tabs: **Real-trace results** (curves and rankings: the main-result slide, live), **How SHADE works**,
**Knapsack solvers**, **DP visualizer** (animated DP table: good for a DAA audience), **Simulation
study (v1)**. If the demo risks failing on stage, use the PNGs in `docs/figures/`.

To re-run the unit tests live: `make lab-test` (builds, then prints "460 checks, 0 failures").
Do **not** try to rerun the full evaluation live: it needs ~3 GB of traces and ~1.5 hours.

---

## 11. Where things are in the repo

| What | Where |
|---|---|
| SHADE policy (≈370 lines) | `lab/src/shade.hpp` (admission margin line 39, exchange test ~line 329) |
| The 8 baselines | `lab/src/policies_basic.hpp`, `lab/src/policies_advanced.hpp` |
| Upper bound PFOO-L | `lab/src/bounds.hpp` |
| Lab tests | `lab/tests/test_lab.cpp` |
| Held-out results (JSON) | `results/lab/*.json` |
| Result tables | `docs/LAB_RESULTS.md` |
| Method write-up | `docs/METHOD.md` |
| v1 report and DAA solvers | `docs/REPORT.md`, `core/src/Knapsack.cpp` |
| Figures | `docs/figures/lab_*.png` (v2), others (v1) |
| Dashboard | `dashboard/` |

**Read before presenting (in this order):** `README.md` → this file → `docs/METHOD.md` (§2–§4) →
skim `docs/LAB_RESULTS.md` "SHADE vs the best baseline" table.
