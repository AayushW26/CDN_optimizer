# Demo script: the dashboard, with what to say

About **6 minutes** (cut the "Knapsack solvers" stop for a 4-minute version).
The numbers and button labels for the "Real-trace results", "How SHADE works" and "DP visualizer" stops were read off the running dashboard.
The "Knapsack solvers" stop is written from the dashboard's code and `docs/REPORT.md`, so glance at that tab once before you present.

**Format:** **[CLICK]** = do something. *Italic quotes* = say it out loud. **[POINT]** = hover or gesture at the screen.

---

## Before you go on (5 min, do not skip)

```bash
make dashboard && make serve      # then open http://127.0.0.1:8787
```

- Start it **before** the talk. `make serve` also builds the C++ engine, which only the old "Live lab" needs. You won't use that.
- Browser zoom **125–150%** and click **light** (top right) for a projector. Full-screen the browser.
- Open the page and confirm the first screen shows **"12 / 15"**. If anything is broken, use the fallback at the bottom.
- Open these in a second window, ready to swap to: `docs/figures/lab_miss_ratio.png` and `docs/figures/lab_improvement.png`.

---

## Stop 1: "Real-trace results" tab (≈ 2:30). This is the main event.

You land here by default.

**[POINT]** at the headline "SHADE on real CDN traffic" paragraph.

> *"Every policy replays the same real request log from Wikipedia's CDN. The important sentence is in the description:
> SHADE and its one tuned constant were developed on a separate dev slice, then frozen and committed **before** the test traces were run."*

**[POINT]** at the four cards, left to right.

> *"First card: SHADE has the fewest misses of all eight baselines in **12 of 15** test settings, that's three traces times five cache sizes."*
>
> *"Second card, and I want to be upfront about it: at small caches, 1% and below, SHADE averages **5.4%** fewer misses than the best baseline.
> At large caches, 3 to 10%, it has **5.9% more**. So it is a small-and-medium-cache policy."*
>
> *"Third card: against plain LRU, the default most systems use, SHADE removes **30%** of misses on average."*
>
> *"Fourth card is the trace itself: 40 million requests, 4.5 million objects, 503 GB, and **55% of objects are requested exactly once**.
> That one fact explains most of the design."*

**[POINT]** at the chart.

> *"Y axis is miss ratio, lower is better. X axis is cache size as a share of everything that was ever requested.
> The blue line is SHADE. The two dashed lines are **offline** references that can see the future, so they're a floor, not competitors.
> At small caches SHADE sits right under GDSF, the strongest baseline, and well under LRU, the purple line at the top."*

**[POINT]** at the "Ranking at 1% cache" card on the left (it defaults to 1%).

> *"Here's one setting in detail. At a 1% cache, SHADE misses 0.235 versus 0.251 for the best baseline, GDSF. That's **6.2% fewer misses**."*

**[CLICK]** the **Wikimedia 2019 text** trace button above the chart.

> *"Different trace: HTML pages, from a different year. 68% of objects are one-hit wonders here. SHADE wins at **every** cache size on this one,
> against a different best baseline, S3-FIFO."*

**[CLICK]** back to **Wikipedia 2018**, then **[CLICK]** the **10%** button in the Ranking card.

> *"Now the honest part. At a 10% cache, GDSF is better by about 24%. We saw this weakness on the dev slice before we ever touched the test data,
> and we reported it instead of retuning on the test set."*

**[POINT]** at the table on the right ("SHADE vs the best baseline, every held-out setting").

> *"All 15 settings are in this table. The red rows are where we lose."*

> **Heads-up about wording on screen.** In that table the "change" column reads **"−6.2% better"**: the minus sign means *fewer misses*.
> Red rows read **"+23.8% worse"**. And at 10% the Ranking sub-line may print a *negative* number next to "fewer misses".
> A negative "fewer misses" means *more* misses. Say it aloud so no one is confused.
>
> Also, the "5.4%" on the card averages all three traces. On the two media traces alone it's **6–8%**. Use whichever you said on your slides, but be consistent.

**[SCROLL]** down to "What each part of SHADE contributes". It starts on Wikipedia 2018 at a 1% cache.

> *"Last thing on this screen: what each component buys us. Each bar is the extra misses when that one part is removed.
> Taking away the whole learned model costs **29%**. Admission control, the part that decides whether a new object is even worth storing, costs **8.6%**.
> Without the censoring correction, **2.5%**."*

**[CLICK]** the **10%** button in that card.

> *"At a 10% cache almost every bar collapses. When the cache is big, these decisions matter much less, which is consistent with where SHADE stops winning."*

---

## Stop 2: "How SHADE works" tab (≈ 1:30)

**[CLICK]** the tab. **[POINT]** at the four-box pipeline.

> *"The whole method in four boxes. **Observe**: every request, cached or not, records how long that object waited since its previous request.
> **Estimate**: from that we learn reuse-time curves. **Index**: we score each object by the best hits per byte per unit time.
> **Decide**: evict the lowest score, and admit a new object only if it beats what it would push out."*

**[POINT]** at card 1, "Caching is a knapsack".

> *"Why it's a knapsack: keeping an object between two requests earns one hit and costs size times gap of cache space-time.
> Choosing which gaps to keep, under a fixed budget, is exactly a knapsack. Its fractional version is solved by the greedy rule from class,
> and that gives the **PFOO-L bound**, the dashed floor you just saw.
> Duality says the optimum is a threshold: keep an interval only if its hits per space-time beat one **shadow price**."*

**[POINT]** at card 4, "Decide".

> *"Online we don't know the gap, so SHADE learns it. And the same number is used twice: to pick what to evict, and to decide whether to admit.
> The admission test is the greedy exchange argument: a newcomer must be worth at least **twice** whatever it would displace.
> That matters because more than half the objects are requested once."*

**[SCROLL]** to "What is new, and what is borrowed".

> *"We're explicit about novelty. The knapsack view comes from PFOO, hit density from LHD, sampled eviction from Redis.
> What's ours is using one learned density for both decisions, learning from objects that aren't in the cache, and the optimal-stopping index.
> And what we did **not** do: no comparison to learned policies like LRB, and no production deployment."*

---

## Stop 3 (optional): "Knapsack solvers" tab (≈ 1:00). Good for a DAA audience.

**[CLICK]** the tab. **[CLICK]** the **memory** button (next to "time") on the first chart.

> *"This is the algorithms side of the project: nine knapsack solvers on the same instances. All three exact dynamic programs take Θ(n·C) time.
> The difference is memory. The full 2-D table reaches **512 MB**. The Hirschberg-style divide-and-conquer version computes the same answer in
> **O(n + C)** space, a few hundred kilobytes, by recomputing halves instead of storing the table."*

**[SCROLL]** to "Hard instances: B&B goes exponential".

> *"And the classic trade-off: branch and bound is fast on typical inputs but explodes on adversarial, strongly-correlated ones,
> while the DP's time just grows smoothly."*

---

## Stop 4: "DP visualizer" tab (≈ 1:30). The crowd-pleaser.

**[CLICK]** the tab. The preset is **CDN mini** (capacity 10; six objects).

> *"Here's the dynamic program on six edge objects. Each cell asks one question: skip this object, or keep it. We take the better of the two."*

**[CLICK]** **Play** and let it run for about 10 seconds. **[POINT]** at the highlighted cells as they move.

> *"The two cells it's looking at in the row above are highlighted. The note below the table spells out the comparison."*

**[CLICK]** **Finish**.

> *"Done. The green path is the backtrack: the best set to cache is hero.jpg, patch.bin and thumbs.zip, total benefit **55**.
> Greedy by value-per-size gets 54, so on this tiny example it's only 2% worse."*

**[CLICK]** **Greedy trap**, then **Finish**.

> *"Here's an example built so greedy fails. It grabs the small, dense item first, and then neither big item fits. Greedy gets **13**.
> The DP gets **20**. That's **35% worse**. That is why exact solvers exist, and why we studied them."*

> *"But on real CDN traffic, as we found, the bottleneck isn't solving the knapsack. It's knowing the future.
> That's what the first half of this demo was about."*

---

## Closing line (10 seconds)

> *"So: caching is a knapsack. We turned the knapsack's optimal answer into an online policy, tested it on 90 million real requests with the
> method frozen in advance, and it wins in 12 of 15 settings. It loses at large caches, and we said so."*

---

## If something breaks

| Problem | Fix |
|---|---|
| Page won't load | Show the PNGs: `docs/figures/lab_miss_ratio.png` (the curves) then `docs/figures/lab_improvement.png` (blue = SHADE better, orange = worse). Same story. |
| Page loads but charts are blank | Hard-refresh (Ctrl/Cmd+Shift+R). Data files are copied by `npm run build`; if they're missing, run `make dashboard` again. |
| Port 8787 in use | `PORT=8790 node server/server.mjs` and open that port. |
| Network or laptop dies entirely | You can talk through the numbers from `docs/PRESENTATION_PREP.md` §6. |

**Don't** open the "Simulation study (v1)" tab live unless asked: its "Live lab" sub-tab needs the C++ engine built and is not part of the main story.
If asked about v1: *"That's the first version of the project, a per-epoch knapsack planner that won on synthetic data but came 4th of 6 on real traces. That failure led to SHADE."*
