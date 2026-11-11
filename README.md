# UniImage Source Code and Experimental Artifacts

This repository contains the **UniImage** prototype, the baseline
configurations, the scripts that run every experiment and generate every
table and figure, and the raw result files (JSON) behind all numbers
reported in the paper.

## Contents

| Directory | Description |
|-----------|-------------|
| `src/` | UniImage C engine (ingest, Huffman-domain synthesis, lazy and incremental serving, cache and GC) |
| `baselines/` | Configuration for each baseline (NAT, DH, SE, RC, ESG, SPL, Chunk-16K) |
| `scripts/` | Experiment scripts (storage, synthesis, latency, cache, GC, correctness) |
| `results/` | Raw experimental output (JSON) backing every number in the paper |

## Dataset

The corpus consists of 338 public Docker Hub images (131.75 GB compressed,
2,638 layers). The repository lists every image and tag in
`scripts/pull_dataset.py` with a script that downloads them from Docker Hub,
so the dataset can be rebuilt without redistributing third-party images:

```bash
python3 scripts/pull_dataset.py --jobs 4
python3 scripts/layer_index.py
```

The resulting `data/images/` directory and `data/layer_index.json` are used
by all experiment scripts.

The registry analysis uses only the published trace statistics cited in
the paper; no third-party trace data is redistributed.

## Building

```bash
cd src && make -j4
```

Dependencies: `zlib`, `libcrypto` (OpenSSL), `libdeflate` (optional),
`libm`.

## Running Experiments

### Storage comparison (Table 2)

```bash
python3 scripts/run_m1_m3.py --policies hybrid --jobs 8
python3 scripts/run_m1_m3.py --policies file --jobs 6
python3 scripts/run_m1_m3.py --policies fixblock --jobs 6
python3 scripts/run_m1_m3.py --policies chunk16k --jobs 3
python3 scripts/run_q1_dedup.py
```

### Synthesis benchmark (Table 3)

```bash
python3 scripts/run_q2_synth.py --jobs 12
python3 scripts/run_e1_fastbaselines.py
```

### Delivery modes (Table 7)

```bash
python3 scripts/run_q4_modes.py       # incremental + lazy
python3 scripts/run_q4_range.py       # sub-stream lazy loading
python3 scripts/run_coldstart.py      # end-to-end cold start
```

### Cache, GC, concurrency

```bash
python3 scripts/run_q6_cache.py       # cache simulation
python3 scripts/run_q7_gc.py           # GC + pinned mode
python3 scripts/run_q8_concurrency.py # concurrency scaling
```

### Correctness

```bash
python3 scripts/run_q9_correctness.py --lazy-ai 4
```

### Figures

```bash
python3 scripts/make_merged_figs.py
python3 scripts/make_figures.py
python3 scripts/make_missing_figs.py
```

## Architecture Overview

UniImage stores each container image layer as:
- A **recipe** recording the exact tar byte layout (headers, PAX records, padding)
- **Content-addressed objects**, each a self-contained Deflate stream

Standard layers are rebuilt by **Huffman-domain synthesis**: decode objects
to recover their LZ77 token streams, concatenate, and re-encode with fresh
Huffman codes. The expensive LZ77 search is performed once at ingest.

## License

See LICENSE file.
