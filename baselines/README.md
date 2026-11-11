# Baseline Configurations

This directory documents how each baseline in the paper is configured and run.

## NAT — Native OCI Registry
Stores layers as received (compressed), deduplicates at layer granularity
by content digest. Uses the same object store as UniImage with
`--policies file` (stores whole layers as objects).

```bash
src/tools/hd_ingest <store> file <layer.tar.gz>
```

## DH — DupHunter (file-level, uncompressed)
File-level deduplication with uncompressed storage.
Approximated by the `file` policy with compression disabled;
raw byte totals are reported in `results/m1_m3.json` under `file_summary.obj_raw_bytes`.

## SE — SimEnc (compressed-segment)
Compressed-segment deduplication using fixed-size blocks.
Implemented as the `fixblock` policy (512 KiB segments):

```bash
src/tools/hd_ingest <store> fixblock --theta 524288 <layer.tar.gz>
```

## RC — Recompress (UniImage store + full recompression)
Uses UniImage's ingest and object store but rebuilds layers by
inflating objects and recompressing monolithically. Measured in the
Q2 synthesis benchmark as the `b4` mode:

```bash
src/tools/hd_synth <store> synth <recipe_sha>   # UniImage
# RC baseline is the b4 mode in results/q2_synth.json
```

## ESG — OCI + eStargz (dual-copy)
Stores every image twice: once as OCI layers, once converted to eStargz.
Storage is 2.02x NAT (both copies layer-deduplicated; eStargz adds ~1% TOC).
Lazy delivery is modeled in `scripts/run_q4_modes.py`.

## SPL — Block Splicing
Reuses raw object blocks without Huffman re-encoding. Measured in
the Q2 benchmark as the `splice` mode:

```bash
src/tools/hd_synth <store> splice <recipe_sha>
```

## Chunk 16K (fixed-size blocks)
Fixed 16 KiB blocks with independent Deflate compression:

```bash
src/tools/hd_ingest <store> chunk16k <layer.tar.gz>
```

## UniImage (hybrid policy)
Default ingest policy: files below sigma are inlined, files between
sigma and theta are batched, files above theta are CDC-chunked:

```bash
src/tools/hd_ingest <store> hybrid <layer.tar.gz>
```
