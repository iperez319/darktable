# Persistent remote worker

`darktable-remote-worker` is the GPL-linked, single-image engine process for the
Exposure + Histogram MVP. It has no listener. A trusted gateway owns the child
process and exchanges the frozen `DTRW` v1 frames over stdin/stdout; diagnostics
go only to stderr.

WP2 and WP3 provide:

- one in-memory image import and one persistent `dt_develop_t` with full,
  preview, and preview2 pipes;
- introspection-checked Exposure v7 state, full-blob baseline/reset semantics,
  intent-aware Exposure/Black coupling, and deterministic state digests;
- serialized revision/generation mutation and rendering;
- checked, mutex-protected backbuffer snapshots normalized to tight, top-first,
  opaque BGRA8 attachments with SHA-256 integrity;
- a GUI-neutral callback over the final host float buffer before gamma packing,
  with a 1,024-bin RGB histogram computed by `dt_histogram_helper`;
- retry-safe histogram replacement plus revision, generation, profile, sampled
  pixel, timing, and pixelpipe-result-digest binding;
- exact framed reads/writes and frozen input/allocation limits.

`surface.rendered.histogram` uses the frozen
`display-referred-float-pre-pack-v1` domain. Its RGB arrays each contain 1,024
bins and sum to `sampledPixels`. The histogram and raw surface share the outer
revision/generation and the same `pixelpipeResultDigest`; framing is unchanged.

## Build

```sh
cmake -B build -DUSE_REMOTE_WORKER=ON -DUSE_MCP=ON
cmake --build build --parallel
```

Building only the `darktable-remote-worker` target is sufficient as a compile
and link check, but it does not stage the complete runtime tree. A worker run
directly from the build directory also needs the RawSpeed camera database and
the loadable image-operation modules (including Exposure), so build the default
target set before running the harness or gateway.

The option defaults to `OFF`. The executable injects `--library :memory:` and
`--conf write_sidecar_files=never` unless those core options are supplied after
`--core`.

## Stability harness

The harness defaults to the persistent-worker exit test: 1,000 alternating
exposure edits, each followed by a verified raw surface, in one worker process.
It checks frame limits, dimensions, ordering metadata, pixel SHA-256, opaque
alpha, histogram shape/totals/source binding, deterministic repeated-state
histograms, and bounded RSS. It also proves that a rejected out-of-range
mutation changes neither the revision nor the generation by successfully
reusing that generation afterward, checks deterministic repeated-state digests
and Exposure-driven Black coupling, and verifies that reset restores the
complete baseline blob digest.

```sh
python3 src/remote/worker_harness.py \
  --worker build/bin/darktable-remote-worker \
  --image /path/to/reference.NEF
```

Use `--edits 3` for a quick smoke test. The default 256-pixel canvas keeps the
stability run focused on lifecycle and allocation behavior rather than final
2048-pixel latency. Pass `--require-histogram-edge-bins` with the committed
synthetic chart to exercise bins 0 and 1023 explicitly.

For a full-size parity check, pass the WP1 decoded-RGB digest for the first
(`--low-ev`) surface with `--expected-first-rgb-sha256`. Use
`--expected-commit` to assert the full source commit advertised in worker
capabilities.

WP7 uses the same path for 10,000 edit/render pairs and emits a
machine-readable latency/RSS curve:

```sh
python3 src/remote/worker_harness.py \
  --worker build/bin/darktable-remote-worker \
  --image ../protocol/v1/fixtures/images/synthetic-reference.png \
  --edits 10000 --width 64 --height 48 --max-long-edge 64 \
  --rss-sample-interval 100 \
  --report-json ../protocol/reference/wp7-worker-stability.json
```

The reported "sequential ceiling" is worker-protocol edit acceptance plus
render response throughput in one process. It excludes gateway, LAN, and iPad
time and is therefore an engine ceiling, not an end-user latency claim. Live
RSS is sampled at the requested interval; the process peak is recorded after
shutdown.
