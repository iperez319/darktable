#!/usr/bin/env python3
"""Exercise one persistent remote-worker process with repeated edit/render pairs."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import random
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
MAJOR = 2
HEADER = struct.Struct(">4sHHIQ")
MAX_JSON = 256 * 1024
MAX_ATTACHMENT = 32 * 1024 * 1024

HELLO = 1
OPEN = 2
SET_EXPOSURE = 3
RESET_EXPOSURE = 4
RENDER = 5
SHUTDOWN = 7
CHECKPOINT_XMP = 8
EXPORT_JPEG = 9
CANCEL = 10
SET_STATE = 11
CAPABILITIES = 0x8001
OPENED = 0x8002
EXPOSURE_ACCEPTED = 0x8003
RENDERED = 0x8004
ARTIFACT_WRITTEN = 0x8005
STATE_ACCEPTED = 0x8006
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


def canonical_state_fixture() -> dict:
    """A deterministic non-default state covering every facade-v3 mapping."""
    return {
        "exposure": {"exposureEV": 0.75, "blackLevel": -0.02},
        "geometry": {
            "crop": {"x": 0.1, "y": 0.1, "width": 0.8, "height": 0.8},
            "rotationQuarterTurns": 1,
            "straightenDegrees": 1.5,
            "flipHorizontal": True,
            "flipVertical": False,
        },
        "whiteBalance": {"temperatureKelvin": 5200.0, "tint": 15.0},
        "light": {
            "contrast": 12.0,
            "highlights": -18.0,
            "shadows": 22.0,
            "whites": 9.0,
            "blacks": -11.0,
        },
        "color": {"vibrance": 17.0, "saturation": -8.0},
        "toneCurve": {
            "points": [
                {"x": 0.0, "y": 0.0},
                {"x": 0.25, "y": 0.2},
                {"x": 0.7, "y": 0.78},
                {"x": 1.0, "y": 1.0},
            ]
        },
        "optics": {
            "lensCorrection": "manual",
            "lensProfile": "Nikkor 24-70mm f/2.8",
            "lensProfileStatus": "applied",
            "chromaticAberrationCorrection": True,
            "defringe": 32.0,
        },
        "detail": {
            "sharpening": 48.0,
            "noiseReduction": 50.0,
            "denoisePreset": "medium",
            "highlightReconstruction": "guidedLaplacians",
        },
        "masks": [
            {
                "id": "mask-subject",
                "name": "Subject refinement",
                "enabled": True,
                "inverted": False,
                "opacity": 0.82,
                "components": [
                    {
                        "id": "gradient-add",
                        "enabled": True,
                        "inverted": False,
                        "operation": "add",
                        "kind": "linearGradient",
                        "gradient": {
                            "start": {"x": 0.5, "y": 0.12},
                            "end": {"x": 0.5, "y": 0.55},
                            "feather": 0.28,
                        },
                        "ellipse": None,
                        "brush": None,
                    },
                    {
                        "id": "ellipse-subtract",
                        "enabled": True,
                        "inverted": True,
                        "operation": "subtract",
                        "kind": "ellipse",
                        "gradient": None,
                        "ellipse": {
                            "center": {"x": 0.52, "y": 0.48},
                            "radiusX": 0.21,
                            "radiusY": 0.31,
                            "rotationDegrees": 12.0,
                            "feather": 0.18,
                        },
                        "brush": None,
                    },
                    {
                        "id": "brush-intersect",
                        "enabled": True,
                        "inverted": False,
                        "operation": "intersect",
                        "kind": "brush",
                        "gradient": None,
                        "ellipse": None,
                        "brush": {
                            "strokes": [
                                {
                                    "id": "stroke-1",
                                    "points": [
                                        {
                                            "x": 0.35,
                                            "y": 0.42,
                                            "radius": 0.035,
                                            "hardness": 0.7,
                                            "opacity": 0.9,
                                            "pressure": 0.45,
                                            "elapsedMilliseconds": 0,
                                        },
                                        {
                                            "x": 0.44,
                                            "y": 0.5,
                                            "radius": 0.052,
                                            "hardness": 0.7,
                                            "opacity": 0.9,
                                            "pressure": 0.78,
                                            "elapsedMilliseconds": 24,
                                        },
                                    ],
                                }
                            ]
                        },
                    },
                ],
                "range": {
                    "enabled": True,
                    "channel": "luminance",
                    "handles": [0.08, 0.22, 0.74, 0.91],
                    "inverted": False,
                },
                "adjustments": {
                    "exposureEV": 0.45,
                    "contrast": 12.0,
                    "highlights": -15.0,
                    "shadows": 10.0,
                    "whites": 4.0,
                    "blacks": -6.0,
                    "vibrance": 14.0,
                    "saturation": 3.0,
                },
            }
        ],
    }


def validate_canonical_state(actual: dict, expected: dict) -> None:
    if actual.get("masks") != expected.get("masks"):
        raise RuntimeError("accepted canonical mask geometry changed")
    exact_paths = (
        ("geometry", "rotationQuarterTurns"),
        ("geometry", "flipHorizontal"),
        ("geometry", "flipVertical"),
        ("optics", "lensCorrection"),
        ("optics", "lensProfile"),
        ("optics", "lensProfileStatus"),
        ("optics", "chromaticAberrationCorrection"),
        ("detail", "denoisePreset"),
        ("detail", "highlightReconstruction"),
    )
    for group, name in exact_paths:
        if actual.get(group, {}).get(name) != expected[group][name]:
            raise RuntimeError(f"accepted canonical state changed {group}.{name}")
    numeric_paths = (
        ("exposure", "exposureEV", 1e-5),
        ("exposure", "blackLevel", 1e-5),
        ("geometry", "straightenDegrees", 1e-5),
        ("whiteBalance", "temperatureKelvin", 2.0),
        ("whiteBalance", "tint", 0.1),
        ("light", "contrast", 1e-4),
        ("light", "highlights", 1e-4),
        ("light", "shadows", 1e-4),
        ("light", "whites", 1e-4),
        ("light", "blacks", 1e-4),
        ("color", "vibrance", 1e-4),
        ("color", "saturation", 1e-4),
        ("optics", "defringe", 1e-4),
        ("detail", "sharpening", 1e-4),
        ("detail", "noiseReduction", 1e-4),
    )
    for group, name, tolerance in numeric_paths:
        if abs(float(actual[group][name]) - float(expected[group][name])) > tolerance:
            raise RuntimeError(
                f"accepted canonical state changed {group}.{name}: "
                f"expected {expected[group][name]}, received {actual[group][name]}"
            )
    for name in ("x", "y", "width", "height"):
        if abs(float(actual["geometry"]["crop"][name]) - expected["geometry"]["crop"][name]) > 1e-5:
            raise RuntimeError(f"accepted canonical state changed geometry.crop.{name}")
    actual_points = actual.get("toneCurve", {}).get("points")
    expected_points = expected["toneCurve"]["points"]
    if not isinstance(actual_points, list) or len(actual_points) != len(expected_points):
        raise RuntimeError("accepted canonical state changed tone-curve point count")
    for actual_point, expected_point in zip(actual_points, expected_points):
        for axis in ("x", "y"):
            if abs(float(actual_point[axis]) - expected_point[axis]) > 1e-5:
                raise RuntimeError(f"accepted canonical state changed toneCurve.{axis}")


def replay_canonical_state(
    worker: Path,
    image: Path,
    root: Path,
    state: dict,
    max_long_edge: int,
    width: int,
    height: int,
) -> tuple[str, str, tuple[int, int]]:
    """Apply and render a complete state in a fresh process for replay parity."""
    command = [
        str(worker),
        "--core",
        "--configdir",
        str(root / "replay-config"),
        "--cachedir",
        str(root / "replay-cache"),
    ]
    process = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
    )
    assert process.stdin is not None and process.stdout is not None
    session_id = str(uuid.uuid4())
    try:
        write_frame(
            process.stdin,
            HELLO,
            envelope(
                "worker.hello",
                str(uuid.uuid4()),
                session_id,
                {"gatewayBuild": "editor-state-replay/1", "protocolMajor": MAJOR},
            ),
        )
        read_frame(process.stdout, CAPABILITIES)
        write_frame(
            process.stdin,
            OPEN,
            envelope(
                "session.open",
                str(uuid.uuid4()),
                session_id,
                {
                    "imagePath": str(image),
                    "maxLongEdge": max_long_edge,
                    "histogramBins": 1024,
                    "colorContract": "srgb-sdr-surface-v1",
                },
            ),
        )
        read_frame(process.stdout, OPENED)
        write_frame(
            process.stdin,
            SET_STATE,
            envelope(
                "session.setState",
                str(uuid.uuid4()),
                session_id,
                {"generation": 1, "state": state},
            ),
        )
        accepted, attachment = read_frame(process.stdout, STATE_ACCEPTED)
        if attachment:
            raise RuntimeError("replayed state acceptance carried an attachment")
        revision = int(accepted["body"]["revision"])
        write_frame(
            process.stdin,
            RENDER,
            envelope(
                "surface.render",
                str(uuid.uuid4()),
                session_id,
                {
                    "generation": 2,
                    "revision": revision,
                    "role": "overview",
                    "normalizedRect": {
                        "x": 0.0,
                        "y": 0.0,
                        "width": 1.0,
                        "height": 1.0,
                    },
                    "sourcePixelsPerOutputPixel": 1.0,
                    "overscanPixels": 0,
                    "width": width,
                    "height": height,
                },
            ),
        )
        rendered, pixels = read_frame(process.stdout, RENDERED)
        validate_surface(rendered, pixels, 2, revision, False)
        return (
            accepted["body"]["stateDigest"],
            rendered["body"]["pixelDigest"],
            (
                int(accepted["body"]["sourcePixelWidth"]),
                int(accepted["body"]["sourcePixelHeight"]),
            ),
        )
    finally:
        if process.poll() is None:
            try:
                write_frame(
                    process.stdin,
                    SHUTDOWN,
                    envelope(
                        "worker.shutdown",
                        str(uuid.uuid4()),
                        session_id,
                        {"reason": "replay-complete"},
                    ),
                )
                process.stdin.close()
                process.wait(timeout=30)
            except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                process.kill()
                process.wait()


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
    expected_role: str = "overview",
    expected_coverage: dict[str, float] | None = None,
) -> str:
    body = message["body"]
    width = int(body["width"])
    height = int(body["height"])
    row_bytes = int(body["bytesPerRow"])
    if body["generation"] != generation or body["revision"] != revision:
        raise RuntimeError("surface ordering metadata does not match the request")
    if not (1 <= width <= 4096 and 1 <= height <= 4096):
        raise RuntimeError("surface dimensions are outside protocol limits")
    if row_bytes != width * 4 or len(pixels) != row_bytes * height:
        raise RuntimeError("surface dimensions and attachment length disagree")
    digest = "sha256:" + hashlib.sha256(pixels).hexdigest()
    if body["pixelDigest"] != digest:
        raise RuntimeError("surface SHA-256 mismatch")
    if pixels[3::4] != b"\xff" * (len(pixels) // 4):
        raise RuntimeError("surface contains non-opaque alpha")
    if expected_coverage is None:
        expected_coverage = {"x": 0.0, "y": 0.0, "width": 1.0, "height": 1.0}
    if body.get("role") != expected_role or body.get("coverage") != expected_coverage:
        raise RuntimeError(
            "surface role or coverage does not match the request: "
            f"expected {expected_role} {expected_coverage}, "
            f"received {body.get('role')} {body.get('coverage')}"
        )
    if int(body.get("sourcePixelWidth", 0)) <= 0 or int(body.get("sourcePixelHeight", 0)) <= 0:
        raise RuntimeError("surface is missing source dimensions")

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


def apply_and_render_state(
    process: subprocess.Popen,
    session_id: str,
    state: dict,
    generation: int,
    width: int,
    height: int,
) -> tuple[dict, bytes]:
    assert process.stdin is not None and process.stdout is not None
    write_frame(
        process.stdin,
        SET_STATE,
        envelope(
            "session.setState",
            str(uuid.uuid4()),
            session_id,
            {"generation": generation, "state": state},
        ),
    )
    accepted, attachment = read_frame(process.stdout, STATE_ACCEPTED)
    if attachment:
        raise RuntimeError("state acceptance unexpectedly carried an attachment")
    revision = int(accepted["body"]["revision"])
    write_frame(
        process.stdin,
        RENDER,
        envelope(
            "surface.render",
            str(uuid.uuid4()),
            session_id,
            {
                "generation": generation,
                "revision": revision,
                "role": "overview",
                "normalizedRect": {"x": 0.0, "y": 0.0, "width": 1.0, "height": 1.0},
                "sourcePixelsPerOutputPixel": 1.0,
                "overscanPixels": 0,
                "width": width,
                "height": height,
            },
        ),
    )
    rendered, pixels = read_frame(process.stdout, RENDERED)
    validate_surface(rendered, pixels, generation, revision, False)
    return rendered, pixels


def validate_local_mask_spatial_effect(
    baseline: bytes,
    masked: bytes,
    width: int,
    height: int,
) -> None:
    if len(baseline) != len(masked) or len(masked) != width * height * 4:
        raise RuntimeError("local-mask locality surfaces have incompatible dimensions")

    def region_differences(x0: float, y0: float, x1: float, y1: float) -> list[int]:
        differences: list[int] = []
        for y in range(int(height * y0), max(int(height * y0) + 1, int(height * y1))):
            for x in range(int(width * x0), max(int(width * x0) + 1, int(width * x1))):
                offset = (y * width + x) * 4
                differences.extend(
                    abs(baseline[offset + channel] - masked[offset + channel])
                    for channel in range(3)
                )
        return differences

    inside = region_differences(0.43, 0.43, 0.57, 0.57)
    outside = []
    for bounds in (
        (0.0, 0.0, 0.12, 0.12),
        (0.88, 0.0, 1.0, 0.12),
        (0.0, 0.88, 0.12, 1.0),
        (0.88, 0.88, 1.0, 1.0),
    ):
        outside.extend(region_differences(*bounds))

    inside_mean = statistics.fmean(inside)
    outside_mean = statistics.fmean(outside)
    outside_changed_fraction = sum(delta != 0 for delta in outside) / len(outside)
    if inside_mean < 1.0:
        raise RuntimeError(
            "local exposure produced no measurable effect inside its ellipse "
            f"(mean color-byte delta {inside_mean:.3f})"
        )
    if outside_mean > max(0.25, inside_mean * 0.05) or outside_changed_fraction > 0.05:
        raise RuntimeError(
            "local exposure escaped its ellipse: "
            f"inside mean delta {inside_mean:.3f}, outside mean delta {outside_mean:.3f}, "
            f"outside changed fraction {outside_changed_fraction:.3%}"
        )


def validate_mask_spatial_probes(
    baseline: bytes,
    masked: bytes,
    width: int,
    height: int,
    changed_probes: list[tuple[float, float]],
    unchanged_probes: list[tuple[float, float]],
    label: str,
) -> None:
    """Require a local adjustment at precise displayed-image coordinates."""
    if len(baseline) != len(masked) or len(masked) != width * height * 4:
        raise RuntimeError(f"{label} spatial surfaces have incompatible dimensions")

    def mean_delta(point: tuple[float, float]) -> float:
        center_x = round(point[0] * (width - 1))
        center_y = round(point[1] * (height - 1))
        radius = max(1, round(min(width, height) * 0.008))
        differences: list[int] = []
        for y in range(max(0, center_y - radius), min(height, center_y + radius + 1)):
            for x in range(max(0, center_x - radius), min(width, center_x + radius + 1)):
                offset = (y * width + x) * 4
                differences.extend(
                    abs(baseline[offset + channel] - masked[offset + channel])
                    for channel in range(3)
                )
        return statistics.fmean(differences)

    changed = [mean_delta(point) for point in changed_probes]
    unchanged = [mean_delta(point) for point in unchanged_probes]
    if min(changed) < 0.5:
        raise RuntimeError(
            f"{label} missed an expected overlay location; probe deltas={changed}"
        )
    if max(unchanged) > max(0.25, min(changed) * 0.08):
        raise RuntimeError(
            f"{label} affected a location outside its overlay; "
            f"changed probe deltas={changed}, unchanged probe deltas={unchanged}"
        )


def local_exposure_mask(component: dict, identifier: str) -> dict:
    return {
        "id": f"{identifier}-mask",
        "name": f"{identifier} spatial regression",
        "enabled": True,
        "inverted": False,
        "opacity": 1.0,
        "components": [component],
        "range": None,
        "adjustments": {
            "exposureEV": 3.0,
            "contrast": 0.0,
            "highlights": 0.0,
            "shadows": 0.0,
            "whites": 0.0,
            "blacks": 0.0,
            "vibrance": 0.0,
            "saturation": 0.0,
        },
    }


def rgb_sha256(pixels: bytes) -> str:
    rgb = bytearray((len(pixels) // 4) * 3)
    rgb[0::3] = pixels[2::4]
    rgb[1::3] = pixels[1::4]
    rgb[2::3] = pixels[0::4]
    return hashlib.sha256(rgb).hexdigest()


def jpeg_dimensions(encoded: bytes) -> tuple[int, int]:
    if not encoded.startswith(b"\xff\xd8"):
        raise RuntimeError("export is not a JPEG")
    offset = 2
    while offset + 4 <= len(encoded):
        if encoded[offset] != 0xFF:
            offset += 1
            continue
        marker = encoded[offset + 1]
        offset += 2
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        length = int.from_bytes(encoded[offset:offset + 2], "big")
        if length < 2 or offset + length > len(encoded):
            break
        if marker in range(0xC0, 0xC4):
            return (
                int.from_bytes(encoded[offset + 5:offset + 7], "big"),
                int.from_bytes(encoded[offset + 3:offset + 5], "big"),
            )
        offset += length
    raise RuntimeError("JPEG dimensions could not be decoded")


def validate_artifact(message: dict, path: Path, expected_kind: str) -> bytes:
    encoded = path.read_bytes()
    body = message["body"]
    digest = "sha256:" + hashlib.sha256(encoded).hexdigest()
    if (
        body.get("kind") != expected_kind
        or int(body.get("bytes", -1)) != len(encoded)
        or body.get("digest") != digest
    ):
        raise RuntimeError(f"{expected_kind} artifact response does not match the file")
    return encoded


def percentile(samples: list[float], fraction: float) -> float:
    """Return a nearest-rank percentile without requiring a third-party package."""
    if not samples:
        return 0.0
    ordered = sorted(samples)
    rank = max(0, min(len(ordered) - 1, int((len(ordered) - 1) * fraction + 0.5)))
    return ordered[rank]


def bootstrap_interval(
    samples: list[float], fraction: float, *, seed: int, iterations: int = 2000
) -> list[float]:
    """Return a deterministic percentile-bootstrap 95% confidence interval."""
    if not samples:
        return [0.0, 0.0]
    rng = random.Random(seed)
    estimates = [
        percentile([rng.choice(samples) for _ in samples], fraction)
        for _ in range(iterations)
    ]
    return [percentile(estimates, 0.025), percentile(estimates, 0.975)]


def metric_summary(samples: list[float], seed: int) -> dict:
    return {
        "count": len(samples),
        "p50": percentile(samples, 0.50),
        "p90": percentile(samples, 0.90),
        "p95": percentile(samples, 0.95),
        "p99": percentile(samples, 0.99),
        "maximum": max(samples, default=0.0),
        "mean": statistics.fmean(samples) if samples else 0.0,
        "standardDeviation": statistics.stdev(samples) if len(samples) > 1 else 0.0,
        "p50Bootstrap95CI": bootstrap_interval(samples, 0.50, seed=seed),
        "p95Bootstrap95CI": bootstrap_interval(samples, 0.95, seed=seed + 1),
    }


def workload_values(args: argparse.Namespace, count: int) -> list[float]:
    if args.workload == "alternating":
        return [args.low_ev if index % 2 == 0 else args.high_ev for index in range(count)]
    if args.workload in ("sweep", "ramp"):
        if count == 1:
            return [args.high_ev]
        return [
            args.low_ev + (args.high_ev - args.low_ev) * index / (count - 1)
            for index in range(count)
        ]
    if args.workload == "random-unique":
        rng = random.Random(args.seed)
        values: list[float] = []
        seen: set[float] = set()
        while len(values) < count:
            value = rng.uniform(args.low_ev, args.high_ev)
            if value not in seen:
                seen.add(value)
                values.append(value)
        return values
    if args.workload == "gesture":
        return [
            args.low_ev
            + (args.high_ev - args.low_ev)
            * (0.5 + 0.5 * math.sin(index * math.tau / 73.0))
            for index in range(count)
        ]
    if args.workload == "no-op":
        return [args.low_ev] * count
    raise ValueError(f"unsupported workload: {args.workload}")


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
            *(["-d", "perf"] if args.worker_debug_perf else []),
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
                         {"gatewayBuild": "editor-foundation-harness/2", "protocolMajor": MAJOR}),
            )
            capabilities, attachment = read_frame(process.stdout, CAPABILITIES)
            if attachment or capabilities["body"]["protocolMajor"] != MAJOR:
                raise RuntimeError("invalid worker capabilities response")
            worker_capabilities = capabilities["body"]
            if capabilities["body"].get("histogramDomains") != [
                "display-referred-float-pre-pack-v1"
            ]:
                raise RuntimeError("worker does not advertise the WP3 histogram domain")
            editor_capabilities = capabilities["body"].get("editorState", {})
            expected_modules = {
                "crop": ("crop", 3),
                "orientation": ("flip", 2),
                "straighten": ("ashift", 5),
                "whiteBalance": ("temperature", 4),
                "contrastVibranceSaturation": ("colorbalancergb", 5),
                "tonalRanges": ("toneequal", 2),
                "toneCurve": ("rgbcurve", 1),
                "lensCorrection": ("lens", 10),
                "chromaticAberration": ("cacorrect", 2),
                "defringe": ("defringe", 1),
                "sharpening": ("sharpen", 1),
                "noiseReduction": ("denoiseprofile", 12),
                "highlightReconstruction": ("highlights", 4),
                "maskLocalAdjustments": ("colorbalancergb", 5),
            }
            if editor_capabilities.get("schemaVersion") != 3:
                raise RuntimeError("worker does not advertise canonical editor-state schema 3")
            if capabilities["body"].get("facadeMappings", {}).get("version") != 3:
                raise RuntimeError("worker does not advertise photographic facade mapping 3")
            for role, (operation, version) in expected_modules.items():
                module = editor_capabilities.get("modules", {}).get(role, {})
                if module.get("operation") != operation or module.get("version") != version:
                    raise RuntimeError(f"worker advertises the wrong {role} module mapping")
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
            baseline_state = opened["body"].get("baselineState")
            if not isinstance(baseline_state, dict) or opened["body"].get("currentState") != baseline_state:
                raise RuntimeError("session.opened is missing matching baseline/current canonical states")
            source_dimensions = (
                int(opened["body"]["sourcePixelWidth"]),
                int(opened["body"]["sourcePixelHeight"]),
            )
            baseline_rss = process_rss_mib(process.pid)
            maximum_rss = baseline_rss
            rss_samples = [] if baseline_rss is None else [{"edit": 0, "rssMiB": baseline_rss}]
            repeated_state_digests: dict[float, str] = {}
            repeated_histogram_digests: dict[float, str] = {}
            round_trip_samples: list[float] = []
            pixelpipe_samples: list[float] = []
            histogram_samples: list[float] = []
            surface_copy_samples: list[float] = []
            split_samples: dict[str, list[float]] = {
                name: [] for name in ("snapshot", "normalize", "digest", "analysis")
            }
            sample_records: list[dict] = []
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

            values = workload_values(args, args.warmups + args.edits)
            capture_dir = Path(args.capture_dir).resolve() if args.capture_dir else None
            if capture_dir:
                capture_dir.mkdir(parents=True, exist_ok=True)

            for overall_index, exposure in enumerate(values):
                measured = overall_index >= args.warmups
                index = overall_index - args.warmups
                round_trip_start = time.monotonic_ns()
                generation = overall_index + 1
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
                            "role": "overview",
                            "normalizedRect": {"x": 0.0, "y": 0.0, "width": 1.0, "height": 1.0},
                            "sourcePixelsPerOutputPixel": 1.0,
                            "overscanPixels": 0,
                            "width": args.width,
                            "height": args.height,
                        },
                    ),
                )
                rendered, pixels = read_frame(process.stdout, RENDERED)
                round_trip_ms = (time.monotonic_ns() - round_trip_start) / 1_000_000.0
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
                if measured and index == 0 and args.expected_first_rgb_sha256:
                    actual_rgb_digest = rgb_sha256(pixels)
                    if actual_rgb_digest != args.expected_first_rgb_sha256:
                        raise RuntimeError(
                            "first worker surface does not match the expected decoded-RGB digest: "
                            f"{actual_rgb_digest}"
                        )
                timing = rendered["body"].get("timingMs", {})
                if measured:
                    round_trip_samples.append(round_trip_ms)
                    pixelpipe_samples.append(float(timing.get("pixelpipe", 0.0)))
                    histogram_samples.append(float(timing.get("histogram", 0.0)))
                    surface_copy_samples.append(float(timing.get("surfaceCopy", 0.0)))
                    for name in split_samples:
                        split_samples[name].append(float(timing.get(name, 0.0)))
                    sample_records.append(
                        {
                            "sample": index,
                            "generation": generation,
                            "revision": revision,
                            "exposureEV": exposure,
                            "roundTripMs": round_trip_ms,
                            **{f"{name}Ms": float(timing.get(name, 0.0)) for name in (
                                "pixelpipe", "histogram", "surfaceCopy", "snapshot",
                                "normalize", "digest", "analysis"
                            )},
                            "pixelDigest": rendered["body"]["pixelDigest"],
                        }
                    )
                    if capture_dir and (
                        index == 0
                        or index + 1 == args.edits
                        or (args.capture_every and (index + 1) % args.capture_every == 0)
                    ):
                        capture_path = capture_dir / f"frame-{index:04d}-g{generation}.bgra"
                        capture_path.write_bytes(pixels)
                        capture_path.with_suffix(".json").write_text(
                            json.dumps(
                                {
                                    "generation": generation,
                                    "revision": revision,
                                    "exposureEV": exposure,
                                    "width": rendered["body"]["width"],
                                    "height": rendered["body"]["height"],
                                    "pixelDigest": rendered["body"]["pixelDigest"],
                                },
                                indent=2,
                                sort_keys=True,
                            )
                            + "\n"
                        )
                if measured and (index % args.rss_sample_interval == 0 or index + 1 == args.edits):
                    rss = process_rss_mib(process.pid)
                    if rss is not None:
                        rss_sample_count += 1
                        maximum_rss = rss if maximum_rss is None else max(maximum_rss, rss)
                        rss_samples.append({"edit": index + 1, "rssMiB": rss})

            # Exercise the desktop-compatible coupling rule, then prove full-blob reset
            # restores the original deterministic digest (including hidden fields).
            generation = args.warmups + args.edits + 1
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

            # The full-input viewport and reduced-input overview must carry the
            # same state/output contract at matching complete-image geometry.
            # Their pixels are measured but are not expected to be identical:
            # darktable deliberately starts those roles from different mipmaps.
            parity_generation = generation + 1
            parity_surfaces: dict[str, bytes] = {}
            parity_messages: dict[str, dict] = {}
            source_scale = max(
                source_dimensions[0] / args.width,
                source_dimensions[1] / args.height,
            )
            for role in ("overview", "viewport"):
                request_id = str(uuid.uuid4())
                write_frame(
                    process.stdin,
                    RENDER,
                    envelope(
                        "surface.render",
                        request_id,
                        session_id,
                        {
                            "generation": parity_generation,
                            "revision": revision,
                            "role": role,
                            "normalizedRect": {
                                "x": 0.0,
                                "y": 0.0,
                                "width": 1.0,
                                "height": 1.0,
                            },
                            "sourcePixelsPerOutputPixel": source_scale,
                            "overscanPixels": 0,
                            "width": args.width,
                            "height": args.height,
                        },
                    ),
                )
                parity, parity_pixels = read_frame(process.stdout, RENDERED)
                validate_surface(
                    parity,
                    parity_pixels,
                    parity_generation,
                    revision,
                    args.require_histogram_edge_bins,
                    expected_role=role,
                )
                parity_surfaces[role] = parity_pixels
                parity_messages[role] = parity
            overview_body = parity_messages["overview"]["body"]
            viewport_body = parity_messages["viewport"]["body"]
            if (
                overview_body["width"] != viewport_body["width"]
                or overview_body["height"] != viewport_body["height"]
                or overview_body["stateDigest"] != viewport_body["stateDigest"]
                or overview_body["moduleStackDigest"] != viewport_body["moduleStackDigest"]
                or overview_body["pixelFormat"] != viewport_body["pixelFormat"]
            ):
                raise RuntimeError(
                    "overview/viewport state, geometry, or output contract does not match: "
                    f"overview={{width: {overview_body['width']}, height: {overview_body['height']}, "
                    f"state: {overview_body['stateDigest']}, stack: {overview_body['moduleStackDigest']}, "
                    f"format: {overview_body['pixelFormat']}}}; "
                    f"viewport={{width: {viewport_body['width']}, height: {viewport_body['height']}, "
                    f"state: {viewport_body['stateDigest']}, stack: {viewport_body['moduleStackDigest']}, "
                    f"format: {viewport_body['pixelFormat']}}}"
                )
            overview_viewport_absolute = [
                abs(left - right)
                for index, (left, right) in enumerate(
                    zip(parity_surfaces["overview"], parity_surfaces["viewport"])
                )
                if index % 4 != 3
            ]
            overview_viewport_delta = {
                "differingColorBytes": sum(
                    value != 0 for value in overview_viewport_absolute
                ),
                "maximum": max(overview_viewport_absolute, default=0),
                "mean": statistics.fmean(overview_viewport_absolute)
                if overview_viewport_absolute
                else 0.0,
                "p95": percentile(overview_viewport_absolute, 0.95),
                "p99": percentile(overview_viewport_absolute, 0.99),
            }

            native_x = (source_dimensions[0] - args.width) // 2
            native_y = (source_dimensions[1] - args.height) // 2
            native_coverage = {
                "x": native_x / source_dimensions[0],
                "y": native_y / source_dimensions[1],
                "width": args.width / source_dimensions[0],
                "height": args.height / source_dimensions[1],
            }
            if args.verify_viewport_cancel:
                canceled_request_id = str(uuid.uuid4())
                write_frame(
                    process.stdin,
                    RENDER,
                    envelope(
                        "surface.render",
                        canceled_request_id,
                        session_id,
                        {
                            "generation": parity_generation + 1,
                            "revision": revision,
                            "role": "viewport",
                            "normalizedRect": native_coverage,
                            "sourcePixelsPerOutputPixel": 1.0,
                            "overscanPixels": 0,
                            "width": args.width,
                            "height": args.height,
                        },
                    ),
                )
                time.sleep(args.viewport_cancel_delay_ms / 1000.0)
                write_frame(
                    process.stdin,
                    CANCEL,
                    envelope(
                        "surface.cancel",
                        str(uuid.uuid4()),
                        session_id,
                        {"generation": parity_generation + 1},
                    ),
                )
                canceled = read_error(process.stdout, "render_superseded")
                if canceled.get("requestId") != canceled_request_id:
                    raise RuntimeError("cancellation response did not identify the superseded render")
            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                RENDER,
                envelope(
                    "surface.render",
                    request_id,
                    session_id,
                    {
                        "generation": parity_generation + 1,
                        "revision": revision,
                        "role": "viewport",
                        "normalizedRect": native_coverage,
                        "sourcePixelsPerOutputPixel": 1.0,
                        "overscanPixels": 0,
                        "width": args.width,
                        "height": args.height,
                    },
                ),
            )
            native, native_pixels = read_frame(process.stdout, RENDERED)
            validate_surface(
                native,
                native_pixels,
                parity_generation + 1,
                revision,
                args.require_histogram_edge_bins,
                expected_role="viewport",
                expected_coverage=native_coverage,
            )
            if args.verify_native_roi_pixels:
                source_width, source_height = source_dimensions
                if (
                    source_width > 4096
                    or source_height > 4096
                    or source_width * source_height * 4 > MAX_ATTACHMENT
                ):
                    raise RuntimeError(
                        "source is too large for the bounded full-surface ROI parity fixture"
                    )
                request_id = str(uuid.uuid4())
                write_frame(
                    process.stdin,
                    RENDER,
                    envelope(
                        "surface.render",
                        request_id,
                        session_id,
                        {
                            "generation": parity_generation + 2,
                            "revision": revision,
                            "role": "viewport",
                            "normalizedRect": {
                                "x": 0.0,
                                "y": 0.0,
                                "width": 1.0,
                                "height": 1.0,
                            },
                            "sourcePixelsPerOutputPixel": 1.0,
                            "overscanPixels": 0,
                            "width": source_width,
                            "height": source_height,
                        },
                    ),
                )
                full, full_pixels = read_frame(process.stdout, RENDERED)
                validate_surface(
                    full,
                    full_pixels,
                    parity_generation + 2,
                    revision,
                    args.require_histogram_edge_bins,
                    expected_role="viewport",
                )
                expected_native = b"".join(
                    full_pixels[
                        ((native_y + row) * source_width + native_x) * 4:
                        ((native_y + row) * source_width + native_x + args.width) * 4
                    ]
                    for row in range(args.height)
                )
                if native_pixels != expected_native:
                    absolute = [
                        abs(left - right)
                        for index, (left, right) in enumerate(
                            zip(native_pixels, expected_native)
                        )
                        if index % 4 != 3
                    ]
                    differing = sum(value != 0 for value in absolute)
                    best_offset = (0, 0)
                    best_mean = statistics.fmean(absolute)
                    for offset_y in range(-4, 5):
                        for offset_x in range(-4, 5):
                            crop_x = native_x + offset_x
                            crop_y = native_y + offset_y
                            if (
                                crop_x < 0
                                or crop_y < 0
                                or crop_x + args.width > source_width
                                or crop_y + args.height > source_height
                            ):
                                continue
                            candidate = b"".join(
                                full_pixels[
                                    ((crop_y + row) * source_width + crop_x) * 4:
                                    ((crop_y + row) * source_width + crop_x + args.width) * 4
                                ]
                                for row in range(args.height)
                            )
                            candidate_delta = [
                                abs(left - right)
                                for index, (left, right) in enumerate(
                                    zip(native_pixels, candidate)
                                )
                                if index % 4 != 3
                            ]
                            candidate_mean = statistics.fmean(candidate_delta)
                            if candidate_mean < best_mean:
                                best_mean = candidate_mean
                                best_offset = (offset_x, offset_y)
                    raise RuntimeError(
                        "native viewport/full-input crop parity failed with "
                        f"{differing} differing color bytes, "
                        f"maximum delta {max(absolute)}, mean delta {statistics.fmean(absolute):.3f}; "
                        f"best local crop offset {best_offset} has mean delta {best_mean:.3f}"
                    )

            # Validate the whole-state facade after the legacy exposure and ROI
            # contracts. First reject a malformed curve without advancing the
            # revision, then apply the same complete state twice and require
            # byte-stable engine/canonical digests.
            state_fixture = canonical_state_fixture()
            invalid_state = json.loads(json.dumps(state_fixture))
            invalid_state["toneCurve"]["points"][1]["x"] = 0.9
            state_generation = parity_generation + 3
            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                SET_STATE,
                envelope(
                    "session.setState",
                    request_id,
                    session_id,
                    {"generation": state_generation, "state": invalid_state},
                ),
            )
            read_error(process.stdout, "invalid_state")

            invalid_mask_state = json.loads(json.dumps(state_fixture))
            invalid_gradient = invalid_mask_state["masks"][0]["components"][0]["gradient"]
            invalid_gradient["end"] = invalid_gradient["start"]
            write_frame(
                process.stdin,
                SET_STATE,
                envelope(
                    "session.setState",
                    str(uuid.uuid4()),
                    session_id,
                    {"generation": state_generation, "state": invalid_mask_state},
                ),
            )
            read_error(process.stdout, "invalid_state")

            next_state_generation = state_generation + 1
            locality_baseline = json.loads(json.dumps(baseline_state))
            locality_baseline["masks"] = []
            _, locality_baseline_pixels = apply_and_render_state(
                process,
                session_id,
                locality_baseline,
                next_state_generation,
                args.width,
                args.height,
            )
            next_state_generation += 1
            locality_masked = json.loads(json.dumps(locality_baseline))
            locality_masked["masks"] = [
                {
                    "id": "locality-ellipse-mask",
                    "name": "Locality regression",
                    "enabled": True,
                    "inverted": False,
                    "opacity": 1.0,
                    "components": [
                        {
                            "id": "locality-ellipse",
                            "enabled": True,
                            "inverted": False,
                            "operation": "add",
                            "kind": "ellipse",
                            "gradient": None,
                            "ellipse": {
                                "center": {"x": 0.5, "y": 0.5},
                                "radiusX": 0.22,
                                "radiusY": 0.22,
                                "rotationDegrees": 0.0,
                                "feather": 0.08,
                            },
                            "brush": None,
                        }
                    ],
                    "range": None,
                    "adjustments": {
                        "exposureEV": 3.0,
                        "contrast": 0.0,
                        "highlights": 0.0,
                        "shadows": 0.0,
                        "whites": 0.0,
                        "blacks": 0.0,
                        "vibrance": 0.0,
                        "saturation": 0.0,
                    },
                }
            ]
            locality_render, locality_masked_pixels = apply_and_render_state(
                process,
                session_id,
                locality_masked,
                next_state_generation,
                args.width,
                args.height,
            )
            validate_local_mask_spatial_effect(
                locality_baseline_pixels,
                locality_masked_pixels,
                int(locality_render["body"]["width"]),
                int(locality_render["body"]["height"]),
            )
            next_state_generation += 1

            # Verify canonical overlay coordinates through crop, straighten,
            # flip, and quarter-turn before reaching darktable raw mask forms.
            crop = {"x": 0.1, "y": 0.1, "width": 0.8, "height": 0.8}
            spatial_baseline = json.loads(json.dumps(locality_baseline))
            spatial_baseline["geometry"] = {
                "crop": crop,
                "rotationQuarterTurns": 1,
                "straightenDegrees": 1.5,
                "flipHorizontal": True,
                "flipVertical": False,
            }
            spatial_baseline["masks"] = []
            _, spatial_baseline_pixels = apply_and_render_state(
                process,
                session_id,
                spatial_baseline,
                next_state_generation,
                args.width,
                args.height,
            )
            next_state_generation += 1

            def displayed(point: tuple[float, float]) -> tuple[float, float]:
                return (
                    (point[0] - crop["x"]) / crop["width"],
                    (point[1] - crop["y"]) / crop["height"],
                )

            gradient_state = json.loads(json.dumps(spatial_baseline))
            gradient_state["masks"] = [local_exposure_mask(
                {
                    "id": "spatial-gradient",
                    "enabled": True,
                    "inverted": False,
                    "operation": "add",
                    "kind": "linearGradient",
                    "gradient": {
                        "start": {"x": 0.5, "y": 0.5},
                        "end": {"x": 0.75, "y": 0.5},
                        "feather": 0.5,
                    },
                    "ellipse": None,
                    "brush": None,
                },
                "gradient",
            )]
            gradient_render, gradient_pixels = apply_and_render_state(
                process,
                session_id,
                gradient_state,
                next_state_generation,
                args.width,
                args.height,
            )
            validate_mask_spatial_probes(
                spatial_baseline_pixels,
                gradient_pixels,
                int(gradient_render["body"]["width"]),
                int(gradient_render["body"]["height"]),
                [displayed((0.75, 0.5))],
                [displayed((0.25, 0.5))],
                "linear gradient",
            )
            next_state_generation += 1

            # Ellipse angles and radii are pixel-space quantities relative to
            # the shorter side, not angles in a normalized-coordinate square.
            full_width = float(source_dimensions[1])
            full_height = float(source_dimensions[0])
            shorter = min(full_width, full_height)
            center = (0.5, 0.5)
            angle = math.radians(32.0)
            probe_radius = 0.11
            major_delta = (
                probe_radius * shorter / full_width * math.cos(angle),
                probe_radius * shorter / full_height * math.sin(angle),
            )
            minor_delta = (
                -probe_radius * shorter / full_width * math.sin(angle),
                probe_radius * shorter / full_height * math.cos(angle),
            )
            ellipse_state = json.loads(json.dumps(spatial_baseline))
            ellipse_state["masks"] = [local_exposure_mask(
                {
                    "id": "spatial-ellipse",
                    "enabled": True,
                    "inverted": False,
                    "operation": "add",
                    "kind": "ellipse",
                    "gradient": None,
                    "ellipse": {
                        "center": {"x": center[0], "y": center[1]},
                        "radiusX": 0.18,
                        "radiusY": 0.045,
                        "rotationDegrees": 32.0,
                        "feather": 0.08,
                    },
                    "brush": None,
                },
                "ellipse",
            )]
            ellipse_render, ellipse_pixels = apply_and_render_state(
                process,
                session_id,
                ellipse_state,
                next_state_generation,
                args.width,
                args.height,
            )
            validate_mask_spatial_probes(
                spatial_baseline_pixels,
                ellipse_pixels,
                int(ellipse_render["body"]["width"]),
                int(ellipse_render["body"]["height"]),
                [
                    displayed(center),
                    displayed((center[0] + major_delta[0], center[1] + major_delta[1])),
                    displayed((center[0] - major_delta[0], center[1] - major_delta[1])),
                ],
                [
                    displayed((center[0] + minor_delta[0], center[1] + minor_delta[1])),
                    displayed((center[0] - minor_delta[0], center[1] - minor_delta[1])),
                ],
                "rotated ellipse",
            )
            next_state_generation += 1

            brush_state = json.loads(json.dumps(spatial_baseline))
            brush_state["masks"] = [local_exposure_mask(
                {
                    "id": "spatial-brush",
                    "enabled": True,
                    "inverted": False,
                    "operation": "add",
                    "kind": "brush",
                    "gradient": None,
                    "ellipse": None,
                    "brush": {
                        "strokes": [{
                            "id": "spatial-stroke",
                            "points": [
                                {
                                    "x": 0.38, "y": 0.45, "radius": 0.018,
                                    "hardness": 1.0, "opacity": 1.0,
                                    "pressure": 1.0, "elapsedMilliseconds": 0,
                                },
                                {
                                    "x": 0.62, "y": 0.55, "radius": 0.018,
                                    "hardness": 1.0, "opacity": 1.0,
                                    "pressure": 1.0, "elapsedMilliseconds": 20,
                                },
                            ],
                        }]
                    },
                },
                "brush",
            )]
            brush_render, brush_pixels = apply_and_render_state(
                process,
                session_id,
                brush_state,
                next_state_generation,
                args.width,
                args.height,
            )
            validate_mask_spatial_probes(
                spatial_baseline_pixels,
                brush_pixels,
                int(brush_render["body"]["width"]),
                int(brush_render["body"]["height"]),
                [displayed((0.38, 0.45)), displayed((0.5, 0.5)), displayed((0.62, 0.55))],
                [displayed((0.38, 0.58)), displayed((0.62, 0.42))],
                "brush stroke",
            )
            next_state_generation += 1

            standalone_range_state = json.loads(json.dumps(spatial_baseline))
            standalone_range_mask = local_exposure_mask({}, "standalone-parametric")
            standalone_range_mask["components"] = []
            standalone_range_mask["range"] = {
                "enabled": True,
                "channel": "luminance",
                "handles": [0.0, 0.0, 1.0, 1.0],
                "inverted": False,
            }
            standalone_range_state["masks"] = [standalone_range_mask]
            _, standalone_range_pixels = apply_and_render_state(
                process,
                session_id,
                standalone_range_state,
                next_state_generation,
                args.width,
                args.height,
            )
            if standalone_range_pixels == spatial_baseline_pixels:
                raise RuntimeError("standalone parametric mask produced no image adjustment")
            next_state_generation += 1

            standalone_range_mask["range"]["inverted"] = True
            _, inverted_range_pixels = apply_and_render_state(
                process,
                session_id,
                standalone_range_state,
                next_state_generation,
                args.width,
                args.height,
            )
            if inverted_range_pixels != spatial_baseline_pixels:
                raise RuntimeError("inverted full parametric range did not exclude the adjustment")
            next_state_generation += 1

            for temperature, tint in ((1901.0, -100.0), (25000.0, 100.0)):
                boundary_state = json.loads(json.dumps(state_fixture))
                boundary_state["whiteBalance"]["temperatureKelvin"] = temperature
                boundary_state["whiteBalance"]["tint"] = tint
                write_frame(
                    process.stdin,
                    SET_STATE,
                    envelope(
                        "session.setState",
                        str(uuid.uuid4()),
                        session_id,
                        {"generation": next_state_generation, "state": boundary_state},
                    ),
                )
                boundary_accepted, attachment = read_frame(process.stdout, STATE_ACCEPTED)
                if attachment:
                    raise RuntimeError("boundary state acceptance carried an attachment")
                validate_canonical_state(boundary_accepted["body"]["state"], boundary_state)
                next_state_generation += 1

            state_acceptances: list[dict] = []
            for repeat in range(2):
                generation = next_state_generation
                next_state_generation += 1
                request_id = str(uuid.uuid4())
                write_frame(
                    process.stdin,
                    SET_STATE,
                    envelope(
                        "session.setState",
                        request_id,
                        session_id,
                        {"generation": generation, "state": state_fixture},
                    ),
                )
                accepted_state, attachment = read_frame(process.stdout, STATE_ACCEPTED)
                if attachment:
                    raise RuntimeError("state acceptance unexpectedly carried an attachment")
                validate_canonical_state(accepted_state["body"]["state"], state_fixture)
                state_acceptances.append(accepted_state)
            first_state_body = state_acceptances[0]["body"]
            second_state_body = state_acceptances[1]["body"]
            if (
                first_state_body["stateDigest"] != second_state_body["stateDigest"]
                or first_state_body["state"] != second_state_body["state"]
            ):
                raise RuntimeError("identical canonical states did not normalize deterministically")
            if int(second_state_body["revision"]) != int(first_state_body["revision"]) + 1:
                raise RuntimeError("canonical state revisions are not strictly sequential")
            revision = int(second_state_body["revision"])
            source_dimensions = (
                int(second_state_body["sourcePixelWidth"]),
                int(second_state_body["sourcePixelHeight"]),
            )
            if source_dimensions[0] <= 0 or source_dimensions[1] <= 0:
                raise RuntimeError("state acceptance is missing developed source dimensions")

            state_render_digests: list[tuple[str, str]] = []
            for repeat in range(2):
                generation = next_state_generation
                next_state_generation += 1
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
                            "role": "overview",
                            "normalizedRect": {
                                "x": 0.0,
                                "y": 0.0,
                                "width": 1.0,
                                "height": 1.0,
                            },
                            "sourcePixelsPerOutputPixel": 1.0,
                            "overscanPixels": 0,
                            "width": args.width,
                            "height": args.height,
                        },
                    ),
                )
                rendered_state, pixels = read_frame(process.stdout, RENDERED)
                histogram_digest = validate_surface(
                    rendered_state,
                    pixels,
                    generation,
                    revision,
                    args.require_histogram_edge_bins,
                )
                state_render_digests.append(
                    (rendered_state["body"]["pixelDigest"], histogram_digest)
                )
            if state_render_digests[0] != state_render_digests[1]:
                raise RuntimeError("canonical state render/histogram output is not deterministic")
            canonical_state_digest = second_state_body["stateDigest"]
            canonical_pixel_digest = state_render_digests[0][0]

            xmp_path = root / "checkpoint.xmp"
            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                CHECKPOINT_XMP,
                envelope(
                    "session.checkpointXmp",
                    request_id,
                    session_id,
                    {"revision": revision, "outputPath": str(xmp_path)},
                ),
            )
            checkpoint, attachment = read_frame(process.stdout, ARTIFACT_WRITTEN)
            if attachment:
                raise RuntimeError("XMP response unexpectedly carried an attachment")
            xmp = validate_artifact(checkpoint, xmp_path, "xmp")
            if b"darktable:" not in xmp:
                raise RuntimeError("checkpoint does not contain darktable XMP state")
            if b"darktable:masks_history" not in xmp or b"Subject refinement" not in xmp:
                raise RuntimeError("checkpoint does not contain canonical mask forms and names")
            for operation in (
                b"crop",
                b"flip",
                b"ashift",
                b"temperature",
                b"colorbalancergb",
                b"toneequal",
                b"rgbcurve",
                b"lens",
                b"cacorrect",
                b"defringe",
                b"sharpen",
                b"denoiseprofile",
                b"highlights",
            ):
                if operation not in xmp:
                    raise RuntimeError(
                        f"checkpoint does not contain canonical {operation.decode()} state"
                    )

            jpeg_path = root / "full-resolution.jpg"
            request_id = str(uuid.uuid4())
            write_frame(
                process.stdin,
                EXPORT_JPEG,
                envelope(
                    "session.exportJpeg",
                    request_id,
                    session_id,
                    {"revision": revision, "outputPath": str(jpeg_path)},
                ),
            )
            exported, attachment = read_frame(process.stdout, ARTIFACT_WRITTEN)
            if attachment:
                raise RuntimeError("JPEG response unexpectedly carried an attachment")
            jpeg = validate_artifact(exported, jpeg_path, "jpeg")
            exported_dimensions = jpeg_dimensions(jpeg)
            if exported_dimensions != source_dimensions:
                raise RuntimeError(
                    "JPEG export is not full-resolution: "
                    f"expected {source_dimensions}, got {exported_dimensions}"
                )

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
            replay_digest, replay_pixel_digest, replay_dimensions = replay_canonical_state(
                worker,
                image,
                root,
                state_fixture,
                args.max_long_edge,
                args.width,
                args.height,
            )
            if (
                replay_digest != canonical_state_digest
                or replay_pixel_digest != canonical_pixel_digest
                or replay_dimensions != source_dimensions
            ):
                raise RuntimeError(
                    "fresh-worker canonical replay did not reproduce state, pixels, and geometry: "
                    f"state={replay_digest == canonical_state_digest}, "
                    f"pixels={replay_pixel_digest == canonical_pixel_digest}, "
                    f"dimensions={replay_dimensions == source_dimensions}; "
                    f"pixelDigest={canonical_pixel_digest}, replayPixelDigest={replay_pixel_digest}"
                )
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
                "schema": "remote-worker-preview-benchmark-v2",
                "worker": {
                    "darktableCommit": worker_capabilities.get("darktableCommit"),
                    "darktableVersion": worker_capabilities.get("darktableVersion"),
                    "protocolMajor": worker_capabilities.get("protocolMajor"),
                },
                "inputSha256": image_digest,
                "baselineStateDigest": baseline_digest,
                "edits": args.edits,
                "warmups": args.warmups,
                "workload": args.workload,
                "seed": args.seed,
                "surface": {"width": args.width, "height": args.height},
                "overviewViewportColorDelta": overview_viewport_delta,
                "exposureEV": {"low": args.low_ev, "high": args.high_ev},
                "sequentialRender": {
                    "roundTripMilliseconds": metric_summary(round_trip_samples, args.seed),
                    "pixelpipeMilliseconds": metric_summary(pixelpipe_samples, args.seed + 10),
                    "histogramMilliseconds": metric_summary(histogram_samples, args.seed + 20),
                    "surfaceCopyMilliseconds": metric_summary(surface_copy_samples, args.seed + 30),
                    "splitMilliseconds": {
                        name: metric_summary(samples, args.seed + 40 + offset)
                        for offset, (name, samples) in enumerate(split_samples.items())
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
                "samples": sample_records,
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
            if args.report_csv:
                csv_path = Path(args.report_csv)
                csv_path.parent.mkdir(parents=True, exist_ok=True)
                with csv_path.open("w", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(sample_records[0]))
                    writer.writeheader()
                    writer.writerows(sample_records)
            print(
                f"PASS: {args.edits} edits/renders in one worker; "
                f"coupling/reset/ROI parity/XMP/full-resolution export verified; final revision={revision}; "
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
    parser.add_argument("--warmups", type=int, default=20)
    parser.add_argument(
        "--workload",
        choices=("alternating", "sweep", "ramp", "random-unique", "gesture", "no-op"),
        default="alternating",
    )
    parser.add_argument("--seed", type=int, default=20260820)
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
    parser.add_argument(
        "--verify-native-roi-pixels",
        action="store_true",
        help="compare a centered 1:1 viewport byte-for-byte with a bounded full-source surface",
    )
    parser.add_argument("--verbose-worker", action="store_true")
    parser.add_argument(
        "--verify-viewport-cancel",
        action="store_true",
        help="supersede one active native viewport render before the parity render",
    )
    parser.add_argument(
        "--viewport-cancel-delay-ms",
        type=float,
        default=0.0,
        help="delay after starting the cancellation fixture render (default: immediate)",
    )
    parser.add_argument(
        "--worker-debug-perf",
        action="store_true",
        help="enable darktable per-module and pixelpipe-cache performance diagnostics",
    )
    parser.add_argument(
        "--report-json",
        help="write machine-readable latency and memory measurements to this path",
    )
    parser.add_argument("--report-csv", help="write one measured sample per CSV row")
    parser.add_argument(
        "--capture-dir",
        help="optionally retain first/last canonical BGRA frames and metadata",
    )
    parser.add_argument(
        "--capture-every",
        type=int,
        default=0,
        help="with --capture-dir, additionally retain every Nth measured frame",
    )
    args = parser.parse_args()
    if args.edits < 1:
        parser.error("--edits must be positive")
    if args.warmups < 0:
        parser.error("--warmups must not be negative")
    if args.capture_every < 0:
        parser.error("--capture-every must not be negative")
    if args.rss_sample_interval < 1:
        parser.error("--rss-sample-interval must be positive")
    if args.viewport_cancel_delay_ms < 0:
        parser.error("--viewport-cancel-delay-ms must not be negative")
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
