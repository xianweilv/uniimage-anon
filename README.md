# UniImage Source Code (Anonymous Submission)

This is the source code for **UniImage**, a registry storage engine that
achieves fine-grained deduplication, always-compressed storage, and fast
standards-compliant layer reconstruction simultaneously.

## Architecture

UniImage stores each container image layer as:
- A **recipe** recording the exact tar byte layout (headers, PAX records, padding)
- **Content-addressed objects**, each a self-contained Deflate stream holding
  file content, a group of small files, or a CDC chunk of a large file

Standard layers are rebuilt by **Huffman-domain synthesis**: decode objects
to recover their LZ77 token streams, concatenate, and re-encode with fresh
Huffman codes. The LZ77 search is performed once at ingest and never repeated.

## Directory Structure

```
src/           Core C engine
  hd_common    Common definitions, Deflate constants
  hd_bitio     Bit-level reader/writer (RFC 1951)
  hd_huffman   Huffman table construction (encode + decode)
  hd_tokdec    Deflate bitstream -> LZ77 tokens (Huffman decode only)
  hd_tokenc    LZ77 tokens -> Deflate blocks (Huffman encode only)
  hd_recipe    Tar stream parsing + recipe serialization
  hd_store     Content-addressed object/recipe store
  hd_ingest    Layer ingest pipeline (gzip -> recipe + objects)
  hd_synth     Layer synthesis engine (Huffman-domain rebuild)
  hd_fastcdc   FastCDC content-defined chunking
  hd_lz77      Standalone LZ77 matcher (for time-split experiments)
  hd_splice    Block splicing ablation baseline
  hd_blkbatch  Block-level dedup + batch compression variant
  libdeflate_min.h  Minimal libdeflate API (for object compression)

  tools/       CLI tools
    hd_ingest  Ingest a layer
    hd_synth   Synthesize a layer (multiple modes)
    hd_tool    Utility CLI (selftest, verify, etc.)
    store_stats  Store statistics

results/       Raw experiment output (JSON)
  m1_m3.json            Storage comparison (Table 2)
  q1_dedup.json         Dedup analysis
  q2_synth.json         Synthesis benchmark (Table 3)
  q3_ingest_pull.json   Ingest throughput, pull latency
  q4_modes.json         Incremental/lazy delivery
  e1_fastbaselines.json Fast compressor comparison
  ...                   (see file headers for details)

scripts/       Experiment scripts
  pull_dataset    Dataset definition and pull
  layer_index     Build layer index from pulled images
  run_m1_m3       Storage comparison experiment (multiple policies)
  run_q1_dedup    Deduplication analysis
  run_q2_synth    Synthesis benchmark
  run_e1_fastbaselines  Fast compressor baselines
  recipe_reader   Recipe parsing + rootfs extraction
  make_merged_figs  Figure generation
```

## Building

```bash
cd src
make -j4
```

Dependencies: `zlib`, `libcrypto` (OpenSSL), `libdeflate` (optional, for
faster ingest), `libm`.

## Usage Examples

```bash
# Ingest a layer
src/tools/hd_ingest <store_dir> hybrid <layer.tar.gz>

# Synthesize (rebuild) a layer
src/tools/hd_synth <store_dir> synth <recipe_sha>

# Verify correctness (diffID check)
src/tools/hd_tool verify <store_dir> <recipe_sha>
```

## License

See LICENSE file.
