#!/usr/bin/env python3
"""Exercise one persistent remote-worker process with repeated edit/render pairs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import resource
import statistics
import struct
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path


MAGIC = b"DTRW"
MAJOR = 1
HEADER = struct.Struct(">4sHHIQ")
MAX_JSON = 256 * 1024
MAX_ATTACHMENT = 16 * 1024 * 1024

HELLO = 1
OPEN = 2
SET_EXPOSURE = 3
RESET_EXPOSURE = 4
RENDER = 5
SHUTDOWN = 7
CAPABILITIES = 0x8001
OPENED = 0x8002
EXPOSURE_ACCEPTED = 0x8003
RENDERED = 0x8004
ERROR = 0xFFFF


def read_exact(stream, size: int) -> bytes:
    chunks: list[bytes] = []
    remaining = size
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            raise EOFError(f"unexpected EOF with {remaining} bytes remaining")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def write_frame(stream, message_type: int, message: dict) -> None:
    encoded = json.dumps(message, separators=(",", ":"), sort_keys=True).encode()
    if not 0 < len(encoded) <= MAX_JSON:
        raise ValueError("JSON frame exceeds worker limit")
    stream.write(HEADER.pack(MAGIC, MAJOR, message_type, len(encoded), 0))
    stream.write(encoded)
    stream.flush()


def read_frame(stream, expected_type: int) -> tuple[dict, bytes]:
    magic, major, message_type, json_size, attachment_size = HEADER.unpack(
        read_exact(stream, HEADER.size)
    )
    if magic != MAGIC or major != MAJOR:
        raise RuntimeError("invalid worker response header")
    if not 0 < json_size <= MAX_JSON or attachment_size > MAX_ATTACHMENT:
        raise RuntimeError("worker response exceeds frozen limits")
    message = json.loads(read_exact(stream, json_size))
    attachment = read_exact(stream, attachment_size)
    if message_type == ERROR:
        body = message.get("body", {})
        raise RuntimeError(f"{body.get('code')}: {body.get('message')}")
    if message_type != expected_type:
        raise RuntimeError(f"expected message type {expected_type:#x}, got {message_type:#x}")
    return message, attachment


def read_error(stream, expected_code: str) -> dict:
    magic, major, message_type, json_size, attachment_size = HEADER.unpack(
        read_exact(stream, HEADER.size)
    )
    if magic != MAGIC or major != MAJOR or message_type != ERROR:
        raise RuntimeError("expected a worker.error frame")
    if not 0 < json_size <= MAX_JSON or attachment_size:
        raise RuntimeError("invalid worker.error frame lengths")
    message = json.loads(read_exact(stream, json_size))
    if message.get("body", {}).get("code") != expected_code:
        raise RuntimeError(f"expected {expected_code}, got {message.get('body', {}).get('code')}")
    return message


def envelope(message_type: str, request_id: str, session_id: str, body: dict) -> dict:
    return {
        "type": message_type,
        "requestId": request_id,
        "sessionId": session_id,
        "body": body,
    }


def process_rss_mib(pid: int) -> float | None:
    try:
        result = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)],
            check=True,
            capture_output=True,
            text=True,
        )
        return int(result.stdout.strip()) / 1024.0
    except (OSError, ValueError, subprocess.SubprocessError):
        return None


def validate_surface(
    message: dict,
    pixels: bytes,
    generation: int,
    revision: int,
    require_histogram_edge_bins: bool,
) -> str:
    body = message["body"]
    width = int(body["width"])
    height = int(body["height"])
    row_bytes = int(body["bytesPerRow"])
    if body["generation"] != generation or body["revision"] != revision:
        raise RuntimeError("surface ordering metadata does not match the request")
    if not (1 <= width <= 2048 and 1 <= height <= 2048):
        raise RuntimeError("surface dimensions are outside protocol limits")
    if row_bytes != width * 4 or len(pixels) != row_bytes * height:
        raise RuntimeError("surface dimensions and attachment length disagree")
    digest = "sha256:" + hashlib.sha256(pixels).hexdigest()
    if body["pixelDigest"] != digest:
        raise RuntimeError("surface SHA-256 mismatch")
    if pixels[3::4] != b"\xff" * (len(pixels) // 4):
        raise RuntimeError("surface contains non-opaque alpha")

    result_digest = body.get("pixelpipeResultDigest", "")
    if not (
        isinstance(result_digest, str)
        and result_digest.startswith("sha256:")
        and len(result_digest) == 71
    ):
        raise RuntimeError("surface has an invalid pixelpipe result digest")
    histogram = body.get("histogram")
    if not isinstance(histogram, dict):
        raise RuntimeError("surface is missing its coherent histogram")
    if (
        histogram.get("domain") != "display-referred-float-pre-pack-v1"
        or histogram.get("bins") != 1024
        or histogram.get("channels") != "rgb"
        or histogram.get("sourceGeneration") != generation
        or histogram.get("sourcePixelpipeResultDigest") != result_digest
    ):
        raise RuntimeError("histogram contract or source binding is invalid")
    sampled_pixels = int(histogram.get("sampledPixels", -1))
    if sampled_pixels != width * height:
        raise RuntimeError("histogram sampled-pixel count does not match the surface")
    channels: list[list[int]] = []
    for name in ("red", "green", "blue"):
        channel = histogram.get(name)
        if (
            not isinstance(channel, list)
            or len(channel) != 1024
            or any(not isinstance(value, int) or value < 0 for value in channel)
        ):
            raise RuntimeError(f"histogram {name} channel is invalid")
        if sum(channel) != sampled_pixels:
            raise RuntimeError(f"histogram {name} total does not match sampled pixels")
        channels.append(channel)
    edge_total = sum(channel[0] + channel[-1] for channel in channels)
    if require_histogram_edge_bins and edge_total == 0:
        raise RuntimeError("histogram edge-bin fixture produced no edge samples")
    encoded_counts = b"".join(
        struct.pack(">I", value) for channel in channels for value in channel
    )
    return hashlib.sha256(encoded_counts).hexdigest()


def rgb_sha256(pixels: bytes) -> str:
    rgb = bytearray((len(pixels) // 4) * 3)
    rgb[0::3] = pixels[2::4]
    rgb[1::3] = pixels[1::4]
    rgb[2::3] = pixels[0::4]
    return hashlib.sha256(rgb).hexdigest()


def percentile(samples: list[float], fraction: float) -> float:
    """Return a nearest-rank percentile without requiring a third-party package."""
    if not samples:
        return 0.0
    ordered = sorted(samples)
    rank = max(0, min(len(ordered) - 1, int((len(ordered) - 1) * fraction + 0.5)))
    return ordered[rank]


def run(args: argparse.Namespace) -> int:
    worker = Path(args.worker).resolve()
    image = Path(args.image).resolve()
    if not worker.is_file() or not os.access(worker, os.X_OK):
        raise ValueError(f"worker is not executable: {worker}")
    if not image.is_file():
        raise ValueError(f"image is not a regular file: {image}")
    if args.width > args.max_long_edge or args.height > args.max_long_edge:
        raise ValueError("render dimensions exceed --max-long-edge")
    with image.open("rb") as image_stream:
        image_digest = hashlib.file_digest(image_stream, "sha256").hexdigest()

    with tempfile.TemporaryDirectory(prefix="darktable-remote-harness.") as temporary:
        root = Path(temporary)
        command = [
            str(worker),
            "--core",
            "--configdir",
            str(root / "config"),
            "--cachedir",
            str(root / "cache"),
        ]
        stderr = None if args.verbose_worker else subprocess.DEVNULL
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr)
        assert process.stdin is not None and process.stdout is not None
        session_id = str(uuid.uuid4())

        try:
            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                HELLO,
                envelope("worker.hello", request_id, session_id,
                         {"gatewayBuild": "wp2-harness/1", "protocolMajor": 1}),
            )
            capabilities, attachment = read_frame(process.stdout, CAPABILITIES)
            if attachment or capabilities["body"]["protocolMajor"] != 1:
                raise RuntimeError("invalid worker capabilities response")
            worker_capabilities = capabilities["body"]
            if capabilities["body"].get("histogramDomains") != [
                "display-referred-float-pre-pack-v1"
            ]:
                raise RuntimeError("worker does not advertise the WP3 histogram domain")
            if (
                args.expected_commit
                and capabilities["body"].get("darktableCommit") != args.expected_commit
            ):
                raise RuntimeError("worker capabilities report the wrong darktable commit")

            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                OPEN,
                envelope(
                    "session.open",
                    request_id,
                    session_id,
                    {
                        "imagePath": str(image),
                        "maxLongEdge": args.max_long_edge,
                        "histogramBins": 1024,
                        "colorContract": "srgb-sdr-surface-v1",
                    },
                ),
            )
            opened, attachment = read_frame(process.stdout, OPENED)
            if attachment:
                raise RuntimeError("session.opened unexpectedly carried an attachment")
            revision = int(opened["body"]["revision"])
            baseline_black = float(opened["body"]["baselineValues"]["black"])
            baseline_exposure = float(opened["body"]["baselineValues"]["exposureEV"])
            baseline_digest = opened["body"]["stateDigest"]
            baseline_rss = process_rss_mib(process.pid)
            maximum_rss = baseline_rss
            rss_samples = [] if baseline_rss is None else [{"edit": 0, "rssMiB": baseline_rss}]
            repeated_state_digests: dict[float, str] = {}
            repeated_histogram_digests: dict[float, str] = {}
            round_trip_samples: list[float] = []
            pixelpipe_samples: list[float] = []
            histogram_samples: list[float] = []
            surface_copy_samples: list[float] = []
            rss_sample_count = 1 if baseline_rss is not None else 0

            # An invalid mutation must not advance either revision or generation.
            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                SET_EXPOSURE,
                envelope(
                    "session.setExposure",
                    request_id,
                    session_id,
                    {
                        "changedField": "exposureEV",
                        "exposureEV": 19.0,
                        "black": baseline_black,
                        "generation": 1,
                    },
                ),
            )
            read_error(process.stdout, "invalid_exposure")

            for index in range(args.edits):
                round_trip_start = time.monotonic_ns()
                generation = index + 1
                exposure = args.low_ev if index % 2 == 0 else args.high_ev
                request_id = str(uuid.uuid4())
                write_frame(
                    process.stdin,
                    SET_EXPOSURE,
                    envelope(
                        "session.setExposure",
                        request_id,
                        session_id,
                        {
                            "changedField": "exposureEV",
                            "exposureEV": exposure,
                            "black": baseline_black,
                            "generation": generation,
                        },
                    ),
                )
                accepted, attachment = read_frame(process.stdout, EXPOSURE_ACCEPTED)
                if attachment:
                    raise RuntimeError("exposure acceptance unexpectedly carried an attachment")
                revision = int(accepted["body"]["revision"])
                state_digest = accepted["body"]["stateDigest"]
                previous_digest = repeated_state_digests.setdefault(exposure, state_digest)
                if state_digest != previous_digest:
                    raise RuntimeError("identical accepted exposure blobs produced different state digests")

                request_id = str(uuid.uuid4())
                write_frame(
                    process.stdin,
                    RENDER,
                    envelope(
                        "surface.render",
                        request_id,
                        session_id,
                        {
                            "generation": generation,
                            "revision": revision,
                            "width": args.width,
                            "height": args.height,
                        },
                    ),
                )
                rendered, pixels = read_frame(process.stdout, RENDERED)
                round_trip_samples.append((time.monotonic_ns() - round_trip_start) / 1_000_000.0)
                histogram_digest = validate_surface(
                    rendered,
                    pixels,
                    generation,
                    revision,
                    args.require_histogram_edge_bins,
                )
                previous_histogram = repeated_histogram_digests.setdefault(
                    exposure, histogram_digest
                )
                if histogram_digest != previous_histogram:
                    raise RuntimeError(
                        "identical exposure states produced different histograms"
                    )
                if index == 0 and args.expected_first_rgb_sha256:
                    actual_rgb_digest = rgb_sha256(pixels)
                    if actual_rgb_digest != args.expected_first_rgb_sha256:
                        raise RuntimeError(
                            "first worker surface does not match the expected decoded-RGB digest: "
                            f"{actual_rgb_digest}"
                        )
                timing = rendered["body"].get("timingMs", {})
                pixelpipe_samples.append(float(timing.get("pixelpipe", 0.0)))
                histogram_samples.append(float(timing.get("histogram", 0.0)))
                surface_copy_samples.append(float(timing.get("surfaceCopy", 0.0)))
                if index % args.rss_sample_interval == 0 or index + 1 == args.edits:
                    rss = process_rss_mib(process.pid)
                    if rss is not None:
                        rss_sample_count += 1
                        maximum_rss = rss if maximum_rss is None else max(maximum_rss, rss)
                        rss_samples.append({"edit": index + 1, "rssMiB": rss})

            # Exercise the desktop-compatible coupling rule, then prove full-blob reset
            # restores the original deterministic digest (including hidden fields).
            generation = args.edits + 1
            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                SET_EXPOSURE,
                envelope(
                    "session.setExposure",
                    request_id,
                    session_id,
                    {
                        "changedField": "exposureEV",
                        "exposureEV": 18.0,
                        "black": 0.5,
                        "generation": generation,
                    },
                ),
            )
            coupled, attachment = read_frame(process.stdout, EXPOSURE_ACCEPTED)
            if attachment:
                raise RuntimeError("coupled exposure acceptance carried an attachment")
            if float(coupled["body"]["values"]["black"]) >= 2.0 ** -18.0:
                raise RuntimeError("exposure-driven Black coupling was not applied")
            revision = int(coupled["body"]["revision"])

            generation += 1
            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                RESET_EXPOSURE,
                envelope(
                    "session.resetExposure",
                    request_id,
                    session_id,
                    {"generation": generation},
                ),
            )
            reset, attachment = read_frame(process.stdout, EXPOSURE_ACCEPTED)
            if attachment:
                raise RuntimeError("reset acceptance carried an attachment")
            reset_values = reset["body"]["values"]
            if (
                float(reset_values["exposureEV"]) != baseline_exposure
                or float(reset_values["black"]) != baseline_black
                or reset["body"]["stateDigest"] != baseline_digest
            ):
                raise RuntimeError("reset did not restore the complete baseline parameter blob")
            revision = int(reset["body"]["revision"])

            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                SHUTDOWN,
                envelope("worker.shutdown", request_id, session_id, {"reason": "harness-complete"}),
            )
            process.stdin.close()
            return_code = process.wait(timeout=30)
            if return_code:
                raise RuntimeError(f"worker exited with status {return_code}")
            peak_raw = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
            peak_rss = peak_raw / (1024.0 * 1024.0) if sys.platform == "darwin" else peak_raw / 1024.0
            limit_failures = []
            if peak_rss > args.max_rss_mib:
                limit_failures.append(
                    f"peak worker RSS was {peak_rss:.1f} MiB, over the {args.max_rss_mib:.1f} MiB limit"
                )
            growth = None
            if baseline_rss is not None and maximum_rss is not None:
                growth = maximum_rss - baseline_rss
                if growth > args.max_rss_growth_mib:
                    limit_failures.append(
                        f"RSS grew {growth:.1f} MiB, over the {args.max_rss_growth_mib:.1f} MiB limit"
                    )
            growth_text = "unavailable" if growth is None else f"{growth:.1f} MiB"
            elapsed_ms = sum(round_trip_samples)
            report = {
                "schema": "remote-worker-stability-v1",
                "worker": {
                    "darktableCommit": worker_capabilities.get("darktableCommit"),
                    "darktableVersion": worker_capabilities.get("darktableVersion"),
                    "protocolMajor": worker_capabilities.get("protocolMajor"),
                },
                "inputSha256": image_digest,
                "edits": args.edits,
                "surface": {"width": args.width, "height": args.height},
                "exposureEV": {"low": args.low_ev, "high": args.high_ev},
                "sequentialRender": {
                    "roundTripMilliseconds": {
                        "p50": percentile(round_trip_samples, 0.50),
                        "p95": percentile(round_trip_samples, 0.95),
                        "maximum": max(round_trip_samples),
                        "mean": statistics.fmean(round_trip_samples),
                    },
                    "pixelpipeMilliseconds": {
                        "p50": percentile(pixelpipe_samples, 0.50),
                        "p95": percentile(pixelpipe_samples, 0.95),
                    },
                    "histogramMilliseconds": {
                        "p50": percentile(histogram_samples, 0.50),
                        "p95": percentile(histogram_samples, 0.95),
                    },
                    "surfaceCopyMilliseconds": {
                        "p50": percentile(surface_copy_samples, 0.50),
                        "p95": percentile(surface_copy_samples, 0.95),
                    },
                    "maximumCompletedEditsPerSecond": (
                        args.edits * 1000.0 / elapsed_ms if elapsed_ms else 0.0
                    ),
                },
                "memoryMiB": {
                    "baseline": baseline_rss,
                    "maximumSampled": maximum_rss,
                    "sampledGrowth": growth,
                    "peakChild": peak_rss,
                    "sampleCount": rss_sample_count,
                    "samples": rss_samples,
                },
                "finalRevision": revision,
                "limits": {
                    "maximumRssMiB": args.max_rss_mib,
                    "maximumRssGrowthMiB": args.max_rss_growth_mib,
                    "passed": not limit_failures,
                    "failures": limit_failures,
                },
            }
            if args.report_json:
                report_path = Path(args.report_json)
                report_path.parent.mkdir(parents=True, exist_ok=True)
                report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
            print(
                f"PASS: {args.edits} edits/renders in one worker; "
                f"coupling/reset verified; final revision={revision}; "
                f"peak child RSS={peak_rss:.1f} MiB; "
                f"RSS growth={growth_text}; "
                f"sequential round-trip p50={report['sequentialRender']['roundTripMilliseconds']['p50']:.2f} ms, "
                f"p95={report['sequentialRender']['roundTripMilliseconds']['p95']:.2f} ms, "
                f"ceiling={report['sequentialRender']['maximumCompletedEditsPerSecond']:.1f} edits/s"
            )
            if limit_failures:
                raise RuntimeError("; ".join(limit_failures))
            return 0
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", required=True, help="path to darktable-remote-worker")
    parser.add_argument("--image", required=True, help="fixed RAW/JPEG/PNG input")
    parser.add_argument("--edits", type=int, default=1000)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--height", type=int, default=256)
    parser.add_argument("--max-long-edge", type=int, default=256)
    parser.add_argument("--low-ev", type=float, default=-2.0)
    parser.add_argument("--high-ev", type=float, default=2.0)
    parser.add_argument("--max-rss-growth-mib", type=float, default=256.0)
    parser.add_argument("--max-rss-mib", type=float, default=2048.0)
    parser.add_argument(
        "--rss-sample-interval",
        type=int,
        default=100,
        help="sample live worker RSS every N edits (default: 100)",
    )
    parser.add_argument(
        "--expected-first-rgb-sha256",
        help="optional decoded-RGB SHA-256 for the first (low-EV) surface",
    )
    parser.add_argument("--expected-commit", help="optional exact 40-hex worker source commit")
    parser.add_argument(
        "--require-histogram-edge-bins",
        action="store_true",
        help="require the test image/state to exercise histogram bins 0 or 1023",
    )
    parser.add_argument("--verbose-worker", action="store_true")
    parser.add_argument(
        "--report-json",
        help="write machine-readable latency and memory measurements to this path",
    )
    args = parser.parse_args()
    if args.edits < 1:
        parser.error("--edits must be positive")
    if args.rss_sample_interval < 1:
        parser.error("--rss-sample-interval must be positive")
    for name in ("width", "height", "max_long_edge"):
        if not 1 <= getattr(args, name) <= 2048:
            parser.error(f"--{name.replace('_', '-')} must be in 1...2048")
    return args


if __name__ == "__main__":
    try:
        raise SystemExit(run(parse_args()))
    except (EOFError, OSError, RuntimeError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
