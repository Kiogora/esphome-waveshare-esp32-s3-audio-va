#!/usr/bin/env python3
"""Coordinate a SystemView trace with a Sendspin playback stall.

This tool watches the device's Sendspin playback buffer over the ESPHome native
API (Wi-Fi) and drives a running OpenOCD instance over its telnet port. When the
buffer freezes -- the backlog stops changing, e.g. stuck at ~18 KiB capacity --
it starts a SEGGER SystemView capture, lets it run a few more seconds so the
trace spans the stall, then stops and writes a context file next to the two
per-core trace files.

By default tracing starts only once a stall is detected, so playback is not
perturbed before the stall. Pass ``--pre-roll`` to start tracing at play time
instead; that captures the stall onset but keeps SystemView streaming during
playback, which can itself induce audio stutter and make the capture misleading.

Expected setup:

  1. The device is on Wi-Fi and reachable on the native API (no encryption).
  2. OpenOCD is already running, e.g.:

       ~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20250707/openocd-esp32/bin/openocd \\
         -f board/esp32s3-builtin.cfg

  3. Start this script, then control the player yourself (play/pause/seek).

For each playback session the script produces three files in ``--output-dir``:

  capture_<UTC timestamp>_pro.svdat   PRO core trace
  capture_<UTC timestamp>_app.svdat   APP core trace
  capture_<UTC timestamp>.json        context: timestamps, buffer/track snapshot,
                                      stall outcome (stall / no_stall)

The ESP-IDF SystemView mapping note still applies when opening the files in the
SystemView GUI: use IDF's ``tools/esp_app_trace/SYSVIEW_FreeRTOS.txt``.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import time
from collections import deque
from datetime import datetime, timedelta, timezone
from pathlib import Path

from aioesphomeapi import (
    APIClient,
    MediaPlayerInfo,
    MediaPlayerState,
    RequiresEncryptionAPIError,
)


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


class OpenOCDClient:
    """Minimal telnet client for OpenOCD's command interface."""

    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.reader: asyncio.StreamReader | None = None
        self.writer: asyncio.StreamWriter | None = None

    async def connect(self) -> None:
        self.reader, self.writer = await asyncio.open_connection(self.host, self.port)
        # Drain the startup banner and prompt.
        try:
            await asyncio.wait_for(self.reader.read(65536), timeout=2.0)
        except asyncio.TimeoutError:
            pass

    async def send(self, command: str, settle: float = 0.4) -> str:
        if self.writer is None or self.reader is None:
            raise RuntimeError("OpenOCD client is not connected")
        self.writer.write(command.encode() + b"\n")
        await self.writer.drain()
        await asyncio.sleep(settle)
        out = b""
        try:
            while True:
                chunk = await asyncio.wait_for(self.reader.read(65536), timeout=1.0)
                if not chunk:
                    break
                out += chunk
                if out.rstrip().endswith(b">"):
                    break
        except asyncio.TimeoutError:
            pass
        return out.decode(errors="replace")

    async def close(self) -> None:
        if self.writer is not None:
            self.writer.close()
            try:
                await self.writer.wait_closed()
            except (BrokenPipeError, ConnectionResetError):
                pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Capture SystemView traces around Sendspin buffer stalls."
    )
    parser.add_argument("--device", default="192.168.100.37",
                        help="ESPHome device address (IP or mDNS name).")
    parser.add_argument("--api-port", type=int, default=6053,
                        help="ESPHome native API port.")
    parser.add_argument("--api-key",
                        help="Base64 API encryption key (noise PSK) for encrypted devices.")
    parser.add_argument("--openocd-host", default="127.0.0.1")
    parser.add_argument("--openocd-port", type=int, default=4444)
    parser.add_argument("--output-dir", default="sysview-stalls",
                        help="Directory for trace files and context JSON.")
    parser.add_argument("--poll-interval", type=float, default=0.25,
                        help="Seconds between buffer samples.")
    parser.add_argument("--stall-seconds", type=float, default=2.0,
                        help="How long the buffer must be unchanged to count as a stall.")
    parser.add_argument("--post-stall-seconds", type=float, default=5.0,
                        help="Keep the trace running this long after a stall is detected.")
    parser.add_argument("--min-backlog-bytes", type=int, default=512,
                        help="Ignore a static buffer at or below this backlog (idle).")
    parser.add_argument("--epsilon-bytes", type=int, default=16,
                        help="Allowed jitter when deciding the buffer is 'unchanged'.")
    parser.add_argument("--max-stalls", type=int, default=1,
                        help="Stop after this many stalls. 0 means keep watching.")
    parser.add_argument("--pre-roll", action="store_true",
                        help="Start tracing at play start instead of at stall detection. "
                             "Captures the stall onset but adds SystemView overhead during "
                             "playback, which can itself cause audio stutter.")
    return parser.parse_args()


class DeviceMonitor:
    """Polls ESPHome state over the native API and exposes the buffer values."""

    def __init__(self, host: str, port: int, noise_psk: str | None = None):
        self.client = APIClient(host, port, None, noise_psk=noise_psk)
        self.name_by_key: dict[int, str] = {}
        self.states: dict[int, object] = {}
        self.media_player_key: int | None = None

    async def connect(self) -> None:
        await self.client.connect(login=True)
        entities, _ = await self.client.list_entities_services()
        self.name_by_key = {e.key: (e.name or "") for e in entities}
        for e in entities:
            if isinstance(e, MediaPlayerInfo):
                self.media_player_key = e.key
                break
        self.client.subscribe_states(self._on_state)
        await asyncio.sleep(2.0)  # let the initial state dump arrive

    def _on_state(self, state) -> None:
        self.states[getattr(state, "key", None)] = state

    def value(self, name: str) -> float | None:
        key = next((k for k, n in self.name_by_key.items() if n == name), None)
        if key is None:
            return None
        state = self.states.get(key)
        if state is None:
            return None
        try:
            return float(state.state)
        except (TypeError, ValueError):
            return None

    def text(self, name: str) -> str:
        key = next((k for k, n in self.name_by_key.items() if n == name), None)
        if key is None:
            return ""
        state = self.states.get(key)
        return str(getattr(state, "state", "") or "")

    def media_player_state(self) -> MediaPlayerState | None:
        if self.media_player_key is None:
            return None
        state = self.states.get(self.media_player_key)
        if state is None:
            return None
        return getattr(state, "state", None)

    async def close(self) -> None:
        await self.client.disconnect()


class CaptureSession:
    """Tracks one OpenOCD SystemView capture (started at playback start)."""

    def __init__(self) -> None:
        self.active = False
        self.stamp: str | None = None
        self.pro_path: Path | None = None
        self.app_path: Path | None = None
        self.started_at: str | None = None

    def trace_files(self) -> dict:
        return {
            "pro": str(self.pro_path.resolve()) if self.pro_path else None,
            "app": str(self.app_path.resolve()) if self.app_path else None,
        }


async def begin_capture(openocd: OpenOCDClient, out_dir: Path,
                        session: CaptureSession, play_started_at: str | None) -> None:
    """Start a pre-roll capture. Call when playback transitions to PLAYING."""
    session.stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    session.pro_path = out_dir / f"capture_{session.stamp}_pro.svdat"
    session.app_path = out_dir / f"capture_{session.stamp}_app.svdat"
    session.started_at = now_iso()

    start_cmd = (f"esp sysview start file://{session.pro_path.resolve()} "
                 f"file://{session.app_path.resolve()}")
    print(f"\n[openocd] {start_cmd}")
    print(await openocd.send(start_cmd))
    session.active = True
    print(f"[trace] capture started at {session.started_at} "
          f"(play began {play_started_at})")


async def end_capture(openocd: OpenOCDClient, out_dir: Path,
                      session: CaptureSession, context: dict) -> Path:
    """Stop the active capture and write the context JSON next to the traces."""
    print("[openocd] esp sysview stop")
    print(await openocd.send("esp sysview stop"))
    session.active = False

    context.setdefault("trace_files", session.trace_files())
    context.setdefault("capture_started_at", session.started_at)
    context.setdefault("generated_at", now_iso())
    context_file = out_dir / f"capture_{session.stamp}.json"
    context_file.write_text(json.dumps(context, indent=2) + "\n", encoding="utf-8")
    print(f"[context] {context_file}")
    return context_file


def detect_stall(samples: deque, stall_seconds: float, poll_interval: float,
                 min_backlog_bytes: int, epsilon_bytes: int) -> bool:
    needed = max(2, int(round(stall_seconds / poll_interval)))
    if len(samples) < needed:
        return False
    window = list(samples)[-needed:]
    values = [v for _, v in window if v is not None]
    if len(values) != len(window):
        return False
    if min(values) < min_backlog_bytes:
        return False
    return (max(values) - min(values)) <= epsilon_bytes


async def main() -> None:
    args = parse_args()
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"[device] connecting to {args.device}:{args.api_port} ...")
    device = DeviceMonitor(args.device, args.api_port, noise_psk=args.api_key)
    try:
        await device.connect()
    except RequiresEncryptionAPIError:
        print("[error] this device requires API encryption; pass the key with --api-key")
        return

    print(f"[openocd] connecting to {args.openocd_host}:{args.openocd_port} ...")
    openocd = OpenOCDClient(args.openocd_host, args.openocd_port)
    try:
        await openocd.connect()
    except OSError as exc:
        print(f"[error] cannot reach OpenOCD: {exc}")
        print("Start OpenOCD first, then rerun this script.")
        await device.close()
        return

    backlog_key = "Sendspin Buffer Backlog Bytes"
    full_key = "Sendspin Buffer Full"
    capacity_key = "Sendspin Buffer Capacity"

    samples: deque = deque()
    stalls = 0
    session = CaptureSession()
    prev_media_state = device.media_player_state()
    play_started_at: str | None = None

    # If the player is already running when we attach, begin pre-roll immediately.
    if args.pre_roll and prev_media_state == MediaPlayerState.PLAYING:
        play_started_at = now_iso()
        print("PLAY ALREADY IN PROGRESS - starting capture")
        await begin_capture(openocd, out_dir, session, play_started_at)

    print("[monitor] watching Sendspin buffer (Ctrl-C to quit)\n")

    try:
        while True:
            backlog = device.value(backlog_key)
            full_pct = device.value(full_key)
            capacity = device.value(capacity_key)
            samples.append((time.monotonic(), backlog))

            media_state = device.media_player_state()

            # Play started: open a fresh capture so the trace includes the lead-up.
            if (media_state == MediaPlayerState.PLAYING
                    and prev_media_state != MediaPlayerState.PLAYING):
                play_started_at = now_iso()
                print("\n" + "-" * 64)
                print(f"PLAY STARTED at {play_started_at}")
                print(f"  track: {device.text('Sendspin Track Artist')} - "
                      f"{device.text('Sendspin Track Title')}")
                print("-" * 64 + "\n")
                if args.pre_roll and not session.active:
                    await begin_capture(openocd, out_dir, session, play_started_at)

            # Playback ended without a stall: finalize the capture.
            elif (args.pre_roll
                  and media_state == MediaPlayerState.IDLE
                  and prev_media_state == MediaPlayerState.PLAYING
                  and session.active):
                print("\nPLAYBACK ENDED - no stall detected; stopping capture\n")
                await end_capture(openocd, out_dir, session, {
                    "device": args.device,
                    "outcome": "no_stall",
                    "play_started_at": play_started_at,
                    "track_artist": device.text("Sendspin Track Artist"),
                    "track_title": device.text("Sendspin Track Title"),
                })

            prev_media_state = media_state

            # Print a live line at most once per second so the STALL banner stays visible.
            now_mono = time.monotonic()
            if not getattr(main, "_last_print", None) or now_mono - main._last_print >= 1.0:
                main._last_print = now_mono
                print(f"[{now_iso()}] backlog={backlog} bytes full={full_pct}% "
                      f"capacity={capacity} bytes")

            if detect_stall(samples, args.stall_seconds, args.poll_interval,
                            args.min_backlog_bytes, args.epsilon_bytes):
                stalls += 1
                detected = now_iso()
                stall_start_estimate = (
                    datetime.now(timezone.utc) - timedelta(seconds=args.stall_seconds)
                ).isoformat(timespec="milliseconds")
                print("\n" + "=" * 64)
                print(f"STALL DETECTED  #{stalls}  at {detected}")
                print(f"  buffer stuck at {backlog} bytes (capacity {capacity})")
                print(f"  full={full_pct}%")
                print(f"  track: {device.text('Sendspin Track Artist')} - "
                      f"{device.text('Sendspin Track Title')}")
                print(f"  progress: {device.value('Sendspin Track Progress')} ms")
                print("=" * 64 + "\n")

                if not session.active:
                    await begin_capture(openocd, out_dir, session, play_started_at)

                # Let the running capture cover a few more seconds past the stall.
                await asyncio.sleep(args.post_stall_seconds)

                context = {
                    "device": args.device,
                    "outcome": "stall",
                    "stall_detected_at": detected,
                    "stall_start_estimate": stall_start_estimate,
                    "buffer_backlog_bytes": backlog,
                    "buffer_full_pct": full_pct,
                    "buffer_capacity_bytes": capacity,
                    "track_artist": device.text("Sendspin Track Artist"),
                    "track_title": device.text("Sendspin Track Title"),
                    "track_progress_ms": device.value("Sendspin Track Progress"),
                    "play_started_at": play_started_at,
                    "short_writes": device.value("Sendspin Short Writes"),
                    "zero_writes": device.value("Sendspin Zero Writes"),
                    "recovery_restarts": device.value("Sendspin Recovery Restarts"),
                    "post_stall_seconds": args.post_stall_seconds,
                    "stall_seconds": args.stall_seconds,
                }
                await end_capture(openocd, out_dir, session, context)
                samples.clear()
                if args.max_stalls and stalls >= args.max_stalls:
                    print(f"[done] captured {stalls} stall trace(s); stopping.")
                    break
                # More stalls expected: start a fresh capture while still playing.
                if args.pre_roll and media_state == MediaPlayerState.PLAYING:
                    await begin_capture(openocd, out_dir, session, play_started_at)

            await asyncio.sleep(args.poll_interval)
    except KeyboardInterrupt:
        print("\n[monitor] interrupted by user")
    finally:
        await device.close()
        await openocd.close()


if __name__ == "__main__":
    asyncio.run(main())
