#!/usr/bin/env python3
"""Exercise one persistent remote-worker process with repeated edit/render pairs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import resource
import struct
import subprocess
import sys
import tempfile
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


def validate_surface(message: dict, pixels: bytes, generation: int, revision: int) -> None:
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


def rgb_sha256(pixels: bytes) -> str:
    rgb = bytearray((len(pixels) // 4) * 3)
    rgb[0::3] = pixels[2::4]
    rgb[1::3] = pixels[1::4]
    rgb[2::3] = pixels[0::4]
    return hashlib.sha256(rgb).hexdigest()


def run(args: argparse.Namespace) -> int:
    worker = Path(args.worker).resolve()
    image = Path(args.image).resolve()
    if not worker.is_file() or not os.access(worker, os.X_OK):
        raise ValueError(f"worker is not executable: {worker}")
    if not image.is_file():
        raise ValueError(f"image is not a regular file: {image}")
    if args.width > args.max_long_edge or args.height > args.max_long_edge:
        raise ValueError("render dimensions exceed --max-long-edge")

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
            repeated_state_digests: dict[float, str] = {}

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
                validate_surface(rendered, pixels, generation, revision)
                if index == 0 and args.expected_first_rgb_sha256:
                    actual_rgb_digest = rgb_sha256(pixels)
                    if actual_rgb_digest != args.expected_first_rgb_sha256:
                        raise RuntimeError(
                            "first worker surface does not match the expected decoded-RGB digest: "
                            f"{actual_rgb_digest}"
                        )
                rss = process_rss_mib(process.pid)
                if rss is not None:
                    maximum_rss = rss if maximum_rss is None else max(maximum_rss, rss)

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
            if peak_rss > args.max_rss_mib:
                raise RuntimeError(
                    f"peak worker RSS was {peak_rss:.1f} MiB, over the {args.max_rss_mib:.1f} MiB limit"
                )
            growth = None
            if baseline_rss is not None and maximum_rss is not None:
                growth = maximum_rss - baseline_rss
                if growth > args.max_rss_growth_mib:
                    raise RuntimeError(
                        f"RSS grew {growth:.1f} MiB, over the {args.max_rss_growth_mib:.1f} MiB limit"
                    )
            growth_text = "unavailable" if growth is None else f"{growth:.1f} MiB"
            print(
                f"PASS: {args.edits} edits/renders in one worker; "
                f"coupling/reset verified; final revision={revision}; "
                f"peak child RSS={peak_rss:.1f} MiB; "
                f"RSS growth={growth_text}"
            )
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
        "--expected-first-rgb-sha256",
        help="optional decoded-RGB SHA-256 for the first (low-EV) surface",
    )
    parser.add_argument("--expected-commit", help="optional exact 40-hex worker source commit")
    parser.add_argument("--verbose-worker", action="store_true")
    args = parser.parse_args()
    if args.edits < 1:
        parser.error("--edits must be positive")
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
