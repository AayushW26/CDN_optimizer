# SHADE: Shadow-price Admission and hit-Density Eviction

SHADE is this project's caching policy for CDN edge servers. It is derived from a knapsack view of
caching and evaluated on real Wikipedia/Wikimedia CDN traces against eight published policies.
This document explains the idea, the derivation, and what is and is not new.

## 1. The problem

An edge cache with capacity `C` bytes sees a stream of requests for objects of very different sizes
(from a few bytes to hundreds of MB). For each request it either serves the object (a hit) or fetches
it from the origin (a miss). On a miss it also decides whether to store the object (**admission**) and,
if space is needed, what to throw out (**eviction**). The goal is the fewest misses (object miss ratio)
or the fewest missed bytes (byte miss ratio).

Real CDN traffic makes this hard: 53–68% of objects in our traces are requested exactly once, sizes span
seven orders of magnitude, and popularity changes over the day.

## 2. Caching is a knapsack over reuse intervals

Look at the trace offline. Every pair of consecutive requests to the same object `(i, next(i))` is a
**reuse interval** of length `D = next(i) − i`. Keeping the object across that interval:

* earns **one hit** (or `size` bytes, for the byte metric);
* uses **`size × D` of cache space-time** (bytes × requests).

Over a run of `N` requests the cache offers `C × N` space-time. Choosing which intervals to cover so as to
maximise hits is a **0/1 knapsack**: items = intervals, value = 1, weight = `size × D`, capacity = `C × N`.
(Real caching adds the constraint that occupancy never exceeds `C` at any instant; dropping that
constraint gives a relaxation.) Its LP relaxation, the **fractional knapsack**, is solved exactly by the
greedy rule from DAA: sort by value/weight and fill. This is the **PFOO-L** bound of Berger, Beckmann and
Harchol-Balter (SIGMETRICS 2018). We implement it (`lab/src/bounds.hpp`) and use it as the upper bound
on what any policy can achieve.

LP duality tells us the shape of the optimum: there is a single **shadow price** `λ` (the dual variable of
the space-time constraint, equal to the density of the "break item") such that an interval is covered
**iff its hit per space-time, `1 / (size × D)`, exceeds `λ`**. The offline optimum is a threshold rule.

## 3. From the offline threshold to an online policy

Online, `D` is unknown when we must decide. SHADE replaces it with a learned distribution and keeps the
threshold structure:

### 3.1 Learning reuse-time distributions (policy-independent)

SHADE remembers, for every object seen in the last `2^23` requests (cached or not), its request count `k`
and the gap between its last two requests `g`. These define a **class**
`c = (bucket(k), log2-bucket(g))` (8 × 26 = 208 classes). Each time an object is requested again,
SHADE records the reuse age `D` in a histogram for the class the object was in at its previous request.

Two statistical details matter on real data (both measured, see the ablations):

* **Right-censoring (Kaplan–Meier).** Objects still waiting for their next request are only known to
  have survived to their current age. Counting only completed reuses biases the estimate towards short
  reuse times. SHADE estimates the hazard `h_c(a) = reuses at age a / objects at risk at age a`, with
  still-open intervals counted as censored at their current age.
* **Consistent forgetting.** Traffic drifts, so old statistics decay with a half-life of `2^22` requests.
  Every interval is weighted by its *start* time, whether it ended in a reuse, was censored, or is still
  open, so decay does not bias long intervals.
* **Empirical-Bayes shrinkage.** Sparse `(class, age)` cells borrow strength from the parent
  (same count bucket, all gaps), which borrows from the global hazard (8 pseudo-observations per level).

Because these statistics include objects that are **not** in the cache, SHADE can value an object before
admitting it. A policy that learns only from its own cached objects (LHD, for example) cannot do this.

### 3.2 The value of keeping an object: an optimal-stopping index

For an object of class `c` that has waited `a` requests since its last request, keeping it for up to `τ`
more requests yields hit probability `P(a < D ≤ τ | D > a)` at expected space-time
`size × E[min(D, τ) − a | D > a]`. SHADE ranks it by the best ratio over every holding time:

```
I(c, a) = max over τ of  P(a < D ≤ τ | D > a) / E[min(D, τ) − a | D > a]
rank(x) = I(class(x), age(x)) / size(x)          (hits per byte per request)
```

This is the online counterpart of the knapsack density `1 / (size × D)`. Taking the max over `τ` (an
optimal-stopping, Gittins-type index) removes the "horizon" parameter a fixed-window estimate needs, and
correctly values reuse patterns with a second peak. The data has one: on Wikipedia, objects seen once show
a rise in reuse probability around 2^17–2^21 requests later, consistent with daily cycles. With a
decreasing hazard the index reduces to the hazard itself. The table `I(c, a)` is refitted every 2^18
requests (O(classes × age-buckets²) ≈ 2M operations).

### 3.3 Eviction

When space is needed, SHADE samples 32 cached objects, merges them with the 32 lowest-ranked candidates
kept from earlier evictions (an eviction pool, as in Redis), and evicts the lowest `rank`. The victim's
rank is the current marginal density, the online estimate of the shadow price `λ`.

### 3.4 Admission: the knapsack exchange test

On a miss with a full cache, SHADE first picks the victims that would make room, then applies the
**exchange argument** from the knapsack greedy proof: swap the new object in only if its value per byte
beats every object it displaces, `rank(new) ≥ 2 × rank(victim)`. Otherwise the cache is left untouched and
the object is served without being stored. The factor 2 is the method's only tuned constant (chosen on the
development slice). A brand-new object's index is an estimate made at age 0, so it must clear the
incumbents by a margin.

Admission matters on CDN traffic: most first requests are for objects that are never requested again.

## 4. What is new, and what is borrowed

Honest positioning. Related work this builds on: GDSF (Cherkasova 1998), AdaptSize (Berger et al.,
NSDI 2017), LHD (Beckmann et al., NSDI 2018), PFOO (Berger et al., SIGMETRICS 2018), LRB (Song et al.,
NSDI 2020), TinyLFU (Einziger et al., ToS 2017), S3-FIFO (Yang et al., SOSP 2023), SIEVE (Zhang et al.,
NSDI 2024), and Akamai's cache-on-second-hit rule (Maggs & Sitaraman, SIGCOMM CCR 2015).

| Component | Source |
|---|---|
| Knapsack/PFOO-L view of caching, threshold structure | Berger et al. 2018 (PFOO); LP duality |
| Hit density (hits per byte per unit time) as a rank | LHD (Beckmann et al., NSDI 2018) |
| Sampled eviction with a pool | Redis; LHD samples too |
| Kaplan–Meier, exponential forgetting, empirical-Bayes shrinkage | standard statistics |
| **Using one learned density both to evict and to admit, through the knapsack exchange test** | this project |
| **Policy-independent reuse statistics (all objects, censoring-corrected), so admission can value uncached objects** | this project |
| **The optimal-stopping index over all holding times, instead of LHD's lifetime-under-current-policy ratio** | this project |

We are not aware of a prior policy that combines these three, but we have not done an exhaustive
literature search. The claim we can support is the measured one: on held-out real traces SHADE has fewer
misses than the eight baselines we implemented in most settings (see `docs/LAB_RESULTS.md`). We have not
compared against learned policies (LRB, GL-Cache, 3L-Cache).

## 5. Costs

* **Per request:** O(1) bookkeeping plus, on eviction, ranking 32–64 candidates (table lookups).
* **Refit:** every 2^18 requests; O(208 × 104²) for the index plus one pass over the history window
  for the censoring counts.
* **Memory:** per object seen in the history window: last-request time, count, class (≈ 12 bytes), plus a
  history queue of 8 bytes per request in the window (2^23 requests ≈ 64 MB). This is more metadata than
  LRU or S3-FIFO and comparable to AdaptSize's per-object statistics. For a production cache the window
  could be bounded by a ghost table; that is not evaluated here.
* **Simulation speed:** 0.3–2 µs per request in our simulator, similar to LHD and faster than AdaptSize.

## 6. Limitations

* Evaluated in simulation on three traces from one operator (Wikimedia), 10–40M requests each.
* Time is logical (request count), as in the LHD and PFOO papers; real deployments see bursts in wall time.
* At the largest cache size (10% of footprint) GDSF remains competitive; SHADE's advantage is largest at
  small and medium caches, where most CDN edges operate relative to their total catalogue.
* No learned baselines (LRB) and no production deployment.
