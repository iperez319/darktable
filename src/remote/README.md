# Persistent remote worker

`darktable-remote-worker` is the GPL-linked, single-image engine process for the
Editor Foundation milestone. It has no listener. A trusted gateway owns the child
process and exchanges `DTRW` v2 frames over stdin/stdout; diagnostics
go only to stderr.

WP2 and WP3 provide:

- one in-memory image import and one persistent `dt_develop_t` with reduced
  overview and full-input viewport pipes; the viewport currently processes the
  full scaled image and crops afterward because the direct-tile parity fixture
  has not passed for the initial RAW stack;
- introspection-checked Exposure v7 state, full-blob baseline/reset semantics,
  intent-aware Exposure/Black coupling, and deterministic state digests;
- serialized revision/generation mutation and rendering;
- a `geometry-preview` surface role used for a whole-image, uncropped crop
  draft proxy; the gateway owns temporary neutral-geometry application and
  exact canonical restoration around that render;
- a generation-scoped, one-way cancellation path that atomically stops a
  superseded viewport pixelpipe while leaving its replacement and session state
  intact;
- checked, mutex-protected backbuffer snapshots normalized to tight, top-first,
  opaque BGRA8 attachments with SHA-256 integrity;
- a GUI-neutral callback over the final host float buffer before gamma packing,
  with a 1,024-bin RGB histogram computed by `dt_histogram_helper`;
- retry-safe histogram replacement plus revision, generation, profile, sampled
  pixel, timing, and pixelpipe-result-digest binding;
- XMP checkpoint and fixed full-resolution JPEG export from reconstructed state;
- exact framed reads/writes and bounded input/allocation limits.

`surface.rendered.histogram` uses the frozen
`display-referred-float-pre-pack-v1` domain. Its RGB arrays each contain 1,024
bins and sum to `sampledPixels`. The histogram and raw surface share the outer
revision/generation and the same `pixelpipeResultDigest`; framing is unchanged.

## Canonical editor state

Private message `session.setState` (`11`) accepts one authoritative state and
returns `session.stateAccepted` (`0x8006`). `session.opened` and
`session.describe` include `baselineState` and `currentState` in addition to the
legacy Exposure `baselineValues`/`currentValues`. Legacy Exposure messages 3
and 4 remain supported and update the Exposure portion of `currentState`.

The worker advertises the schema, ranges, exact operation/version pins, and
mapping names in `worker.capabilities.editorState`. The mappings are:

- Exposure v7: `exposureEV` and `blackLevel` are written directly. Unlike the
  legacy single-field message, whole-state application does not run the
  Exposure/Black gesture-coupling rule.
- Crop v3: oriented-normalized `{x,y,width,height}` becomes
  `{cx=x, cy=y, cw=x+width, ch=y+height}` with free aspect. Width and height
  have the module's 0.01 minimum.
- Orientation (`flip`) v2: `rotationQuarterTurns` is clockwise; horizontal and
  vertical flips are then composed in output axes. The product transform is
  composed over the image's baseline/EXIF orientation, so the zero state does
  not discard camera orientation.
- Rotate and perspective (`ashift`) v5: `straightenDegrees` maps directly to
  `rotation`, with largest-area automatic crop. No perspective correction is
  exposed by this schema.
- Temperature v4: Kelvin is converted to CIE xy with the standard analytic
  blackbody formula below 4000 K and daylight formula at/above 4000 K, then to
  image-specific camera coefficients normalized to green = 1. If an extreme
  Kelvin/tint combination would exceed temperature v4's coefficient maximum,
  all four coefficients are uniformly scaled to a maximum of 8, preserving
  chromaticity at the cost of a global white-balance gain shift. Product tint
  maps piecewise around neutral: for `t >= 0`, engine tint is
  `1 + t/100 * (2.326 - 1)`; otherwise it is
  `1 + t/100 * (1 - 0.135)`. Temperature v4 stores coefficients rather than
  Kelvin/tint, so accepted canonical Kelvin/tint are the normalized source of
  truth; baseline coefficient inversion is approximate and falls back to
  6500 K/zero tint when the source matrix is not invertible.
- Color balance RGB v5: Contrast, Vibrance, and Saturation divide by 100 and
  map to `contrast`, `vibrance`, and `saturation_global`.
- Tone equalizer v2: each signed percentage divides by 50. Blacks drive the
  noise and two deepest bands, Shadows drive the next three bands, Highlights
  drive the highlight band, and Whites drive white/specular bands; midtones are
  held at zero.
- RGB curve v1: 2...20 strictly x-sorted points are installed as the linked
  master curve using monotone Hermite interpolation. Independent channel curves
  are intentionally outside this MVP3 contract.
- Lens correction v10: `off`, `automatic`, and `manual` map to the Lensfun
  method with all distortion/TCA/vignette corrections. Automatic retains the
  image-derived baseline profile; manual installs the bounded submitted profile
  name. An empty resolved profile returns `missing` and leaves the module off.
- Raw chromatic aberrations v2 is an independent enable switch. Defringe v1
  maps 0...100 to `threshold = 128 - 1.275 × amount`; zero disables it.
- Sharpen v1 maps 0...100 to USM amount 0...2. Profiled denoise v12 uses
  wavelets and maps 0...100 to strength 0.25...2; zero disables it and the
  off/low/medium/high presets canonicalize to 0/25/50/75.
- Highlight reconstruction v4 maps the five named product choices to inpaint
  opposed, clip, reconstruct color, guided laplacians, or segmentation.
- Conventional masks use darktable drawn-mask forms in oriented-normalized
  coordinates. Linear gradients map to gradient forms, ellipses to ellipse
  forms, and each brush stroke to a brush form inside a component group so
  component-level inversion and add/subtract/intersect composition remain
  ordered. Each canonical mask owns a duplicated Color Balance RGB v5 instance;
  its opacity/inversion, drawn group, optional parametric channel/four-handle
  range, and local Exposure/Light/Color controls are installed in blend and
  module parameters. Creating an instance rebuilds the pixelpipe topology before
  the first authoritative render.

AI object masks remain outside facade v3. Parameters in the pinned modules that
are not product controls are copied from the session baseline.
The state digest covers the normalized canonical fields, module versions,
enablement, and complete accepted parameter blobs. A geometry commit also
recomputes the developed dimensions from the synchronized full pixelpipe before
acceptance, so subsequent overview, viewport, XMP, and JPEG operations use the
same geometry.

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
complete baseline blob digest. It also checks overview/viewport state,
geometry, and output-contract compatibility, records the expected color delta
between reduced and full inputs, and verifies a true 1:1 viewport ROI, XMP
integrity, and full-developed-dimension JPEG export. Add
`--verify-native-roi-pixels` to compare the viewport bytes exactly with a crop
from a complete native-scale render. Add `--verify-viewport-cancel` to stop an
active native viewport immediately, verify its correlated `render_superseded`
result (including the pre-pixelpipe cancellation race), and then prove the same
worker can render the replacement. Use `--viewport-cancel-delay-ms` to exercise
cancellation later in the pixelpipe as well.

The same run now validates the complete Phase 22 state with a frozen non-default
crop/rotation/straighten/WB/Light/Color/curve and gradient/ellipse/brush mask
fixture. It rejects an invalid curve and invalid gradient before mutation,
applies the fixture twice to prove normalized state and digest stability,
renders twice to prove pixel/histogram stability, verifies every mapped module
and mask form is checkpointed to XMP, checks geometry-aware full-resolution export, and
replays the state in a fresh worker to require identical state, pixels, and
dimensions. Pure mapping/boundary checks can be run without initializing
darktable:

```sh
build/bin/darktable-remote-worker --editor-state-self-test
```

The worker now reports separate `snapshot`, `normalize`, `digest`, and
`analysis` spans while retaining aggregate `surfaceCopy` compatibility. It
emits a structured `privateWrite` event after the attachment flush. The harness
supports `alternating`, `sweep`, `ramp`, `random-unique`, `gesture`, and `no-op`
workloads, unrecorded warmups, deterministic bootstrap confidence intervals,
per-sample CSV/JSON, optional canonical BGRA capture, and
`--worker-debug-perf` per-module/cache diagnostics. For example:

```sh
python3 src/remote/worker_harness.py \
  --worker build-mvp/bin/darktable-remote-worker \
  --image ../sample-images/nikon_24mp.nef \
  --workload random-unique --seed 20260820 \
  --warmups 20 --edits 200 --width 2048 --height 1357 \
  --max-long-edge 2048 --worker-debug-perf \
  --report-json /tmp/worker.json --report-csv /tmp/worker.csv
```

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
