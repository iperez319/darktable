# Persistent remote worker

`darktable-remote-worker` is the GPL-linked, single-image engine process for the
Exposure + Histogram MVP. It has no listener. A trusted gateway owns the child
process and exchanges the frozen `DTRW` v1 frames over stdin/stdout; diagnostics
go only to stderr.

WP2 provides:

- one in-memory image import and one persistent `dt_develop_t` with full,
  preview, and preview2 pipes;
- introspection-checked Exposure v7 state, full-blob baseline/reset semantics,
  intent-aware Exposure/Black coupling, and deterministic state digests;
- serialized revision/generation mutation and rendering;
- checked, mutex-protected backbuffer snapshots normalized to tight, top-first,
  opaque BGRA8 attachments with SHA-256 integrity;
- exact framed reads/writes and frozen input/allocation limits.

The `histogram` member of `surface.rendered` is `null` in WP2 and the worker
advertises no histogram domain. WP3 installs the coherent pre-gamma tap and
populates that member without changing the v1 framing.

## Build

```sh
cmake -B build -DUSE_REMOTE_WORKER=ON -DUSE_MCP=ON
cmake --build build --target darktable-remote-worker
```

The option defaults to `OFF`. The executable injects `--library :memory:` and
`--conf write_sidecar_files=never` unless those core options are supplied after
`--core`.

## Stability harness

The harness defaults to the WP2 exit test: 1,000 alternating exposure edits,
each followed by a verified raw surface, in one worker process. It checks frame
limits, dimensions, ordering metadata, pixel SHA-256, opaque alpha, and bounded
RSS. It also proves that a rejected out-of-range mutation changes neither the
revision nor the generation by successfully reusing that generation afterward,
checks deterministic repeated-state digests and Exposure-driven Black coupling,
and verifies that reset restores the complete baseline blob digest.

```sh
python3 src/remote/worker_harness.py \
  --worker build/bin/darktable-remote-worker \
  --image /path/to/reference.NEF
```

Use `--edits 3` for a quick smoke test. The default 256-pixel canvas keeps the
stability run focused on lifecycle and allocation behavior rather than final
2048-pixel latency.

For a full-size parity check, pass the WP1 decoded-RGB digest for the first
(`--low-ev`) surface with `--expected-first-rgb-sha256`. Use
`--expected-commit` to assert the full source commit advertised in worker
capabilities.
