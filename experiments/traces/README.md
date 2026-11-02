# Traces

`cdnopt run --trace <file.csv>` replays a real access log instead of a synthetic workload.

Accepted CSV headers (first line):

```
object_id,size_kb
timestamp,object_id,size_kb
```

`object_id` can be any string (URL, hash...). It is remapped to dense integer ids, and the catalog
is built from the sizes in the log. Origin tier, price and latency are assigned per object
because logs don't carry them.

Generate a sample: `python3 scripts/make_sample_trace.py` (writes `sample_trace.csv` here, git-ignored).

Public traces that convert easily: the Wikipedia CDN request traces (Song et al., NSDI 2020,
"Learning Relaxed Belady") and web-proxy logs such as the IRCache/NLANR datasets.
