#!/usr/bin/env python3
"""DBC signal generator for testing the Signal Analysis / Viewer tabs.

This script reads a DBC database, then continuously transmits CAN frames whose
bytes are *encoded from the DBC* — every signal is driven by a smooth,
time-varying waveform inside its documented ``[min|max]`` range, packed back
into the message using the signal's bit position, byte order (Intel/Motorola),
sign and ``factor``/``offset`` scaling.

Because the traffic is encoded through the same rules CANoScope uses to
decode it, the physical values shown in the *Signal Analysis* table and plotted
in the *Signal Analysis Viewer* match what this script intends to send — which
makes it a genuine round-trip test of any DBC, including extended-ID frames.

It uses Linux SocketCAN with a virtual interface (``vcan0`` by default), so no
PEAK/PCAN hardware is required.

Examples
--------
List the signals parsed from the bundled demo database::

    python3 scripts/test_dbc.py --list

Generate 60 s of demo traffic on vcan0 (creates vcan0 if needed)::

    python3 scripts/test_dbc.py

Stress the viewer at 200 Hz per message for 2 minutes, then keep going::

    python3 scripts/test_dbc.py --rate 200 --duration 120

Use your own database and launch the app first::

    python3 scripts/test_dbc.py --dbc mybus.dbc --launch-app
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path


# struct can_frame: can_id (u32, host order), can_dlc (u8), 3 pad, data[8]
CAN_FRAME_FMT = "=IB3x8s"
CAN_FRAME_SIZE = struct.calcsize(CAN_FRAME_FMT)
CAN_EFF_FLAG = 0x80000000          # extended (29-bit) frame flag
CAN_EFF_MASK = 0x1FFFFFFF
CAN_SFF_MASK = 0x000007FF

DEFAULT_DBC = "assets/demo.dbc"


class TestError(RuntimeError):
    """Raised for expected, user-facing failures."""


# ------------------------------------------------------------------ #
# DBC model                                                            #
# ------------------------------------------------------------------ #

@dataclass
class Signal:
    name: str
    start_bit: int
    length: int
    little_endian: bool   # True = Intel (@1), False = Motorola (@0)
    signed: bool
    factor: float
    offset: float
    minimum: float
    maximum: float
    unit: str

    def raw_limits(self) -> tuple[int, int]:
        """Inclusive (lo, hi) range of the raw integer for this signal."""
        if self.signed:
            return -(1 << (self.length - 1)), (1 << (self.length - 1)) - 1
        return 0, (1 << self.length) - 1


@dataclass
class Message:
    can_id: int           # raw id with the EFF flag already stripped
    name: str
    dlc: int
    extended: bool
    signals: list[Signal] = field(default_factory=list)


_BO_RE = re.compile(r"^\s*BO_\s+(\d+)\s+(\w+)\s*:\s*(\d+)\s+(\S+)")
_SG_RE = re.compile(
    r"^\s*SG_\s+(\w+)\s*"
    r":\s*(\d+)\|(\d+)@([01])([+-])\s*"
    r"\(([^,]+),([^)]+)\)\s*"
    r"\[([^|]*)\|([^\]]*)\]\s*"
    r'"([^"]*)"'
)


def parse_dbc(path: Path) -> list[Message]:
    """Parse the message/signal definitions out of a DBC file."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise TestError(f"could not read DBC: {exc}") from exc

    messages: list[Message] = []
    current: Message | None = None

    for line in text.splitlines():
        bo = _BO_RE.match(line)
        if bo:
            raw_id = int(bo.group(1))
            extended = bool(raw_id & CAN_EFF_FLAG)
            can_id = raw_id & (CAN_EFF_MASK if extended else CAN_SFF_MASK)
            current = Message(can_id, bo.group(2), int(bo.group(3)), extended)
            messages.append(current)
            continue

        sg = _SG_RE.match(line)
        if sg and current is not None:
            name, start, length, order, sign = sg.group(1, 2, 3, 4, 5)
            factor, offset, lo, hi, unit = sg.group(6, 7, 8, 9, 10)
            signal = Signal(
                name=name,
                start_bit=int(start),
                length=int(length),
                little_endian=(order == "1"),
                signed=(sign == "-"),
                factor=float(factor),
                offset=float(offset),
                minimum=float(lo) if lo.strip() else 0.0,
                maximum=float(hi) if hi.strip() else 0.0,
                unit=unit.strip(),
            )
            # Fall back to a sensible range when the DBC leaves [0|0].
            if signal.maximum <= signal.minimum:
                rlo, rhi = signal.raw_limits()
                signal.minimum = rlo * signal.factor + signal.offset
                signal.maximum = rhi * signal.factor + signal.offset
            current.signals.append(signal)

    if not messages:
        raise TestError(f"no BO_ messages found in {path}")
    return messages


# ------------------------------------------------------------------ #
# Signal encoding (Intel + Motorola)                                   #
# ------------------------------------------------------------------ #

def _motorola_msb_index(start_bit: int) -> int:
    """Index of the signal's MSB within a big-endian 64-bit word.

    Converts a DBC Motorola (sawtooth) start bit — where bit 7 is the MSB of
    byte 0 — into a 0-based index counted from the most-significant bit of an
    8-byte big-endian value.
    """
    byte, bit = divmod(start_bit, 8)
    return byte * 8 + (7 - bit)


def encode_signal(data: int, sig: Signal, raw: int) -> int:
    """Return ``data`` (a 64-bit int, byte 0 = least significant) with ``raw``
    written into ``sig``'s bit field."""
    mask = (1 << sig.length) - 1
    raw &= mask
    if sig.little_endian:
        return data | (raw << sig.start_bit)

    # Motorola: build into a big-endian view, then fold back to little-endian.
    be = int.from_bytes(data.to_bytes(8, "little"), "big")
    shift = 64 - _motorola_msb_index(sig.start_bit) - sig.length
    be |= raw << shift
    return int.from_bytes(be.to_bytes(8, "big"), "little")


def physical_to_raw(sig: Signal, value: float) -> int:
    """Scale a physical value to a clamped raw integer for the signal."""
    raw = round((value - sig.offset) / sig.factor) if sig.factor else 0
    lo, hi = sig.raw_limits()
    return max(lo, min(hi, raw))


def encode_message(msg: Message, t: float, phase: int) -> bytes:
    """Encode all of ``msg``'s signals at time ``t`` into the frame payload."""
    data = 0
    for i, sig in enumerate(msg.signals):
        mid = (sig.maximum + sig.minimum) / 2.0
        amp = (sig.maximum - sig.minimum) / 2.0 * 0.9
        # A distinct slow frequency per signal so traces look varied.
        freq = 0.05 + 0.035 * (phase + i)
        value = mid + amp * math.sin(2.0 * math.pi * freq * t)
        data = encode_signal(data, sig, physical_to_raw(sig, value))
    return data.to_bytes(8, "little")[: msg.dlc]


def pack_frame(msg: Message, payload: bytes) -> bytes:
    raw_id = msg.can_id | (CAN_EFF_FLAG if msg.extended else 0)
    return struct.pack(CAN_FRAME_FMT, raw_id, len(payload), payload.ljust(8, b"\x00"))


# ------------------------------------------------------------------ #
# vcan setup + socket                                                  #
# ------------------------------------------------------------------ #

def _run(cmd: list[str], *, check: bool = True) -> int:
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if check and proc.returncode != 0:
        raise TestError(f"command failed: {' '.join(cmd)}")
    return proc.returncode


def _root(cmd: list[str], *, no_sudo: bool) -> list[str]:
    if os.geteuid() == 0:
        return cmd
    if no_sudo:
        raise TestError("vcan setup needs root; re-run with sudo or --no-setup.")
    for elevator in ("sudo", "pkexec"):
        path = shutil.which(elevator)
        if path:
            return [path, *cmd]
    raise TestError("neither sudo nor pkexec found; create the interface manually.")


def iface_exists(iface: str) -> bool:
    return _run(["ip", "link", "show", "dev", iface], check=False) == 0


def ensure_vcan(iface: str, *, no_setup: bool, no_sudo: bool) -> None:
    if not shutil.which("ip"):
        raise TestError("the 'ip' command was not found.")
    if iface_exists(iface):
        return
    if no_setup:
        raise TestError(f"{iface} does not exist and --no-setup was given.")
    if shutil.which("modprobe"):
        _run(_root(["modprobe", "vcan"], no_sudo=no_sudo), check=False)
    _run(_root(["ip", "link", "add", "dev", iface, "type", "vcan"], no_sudo=no_sudo))
    _run(_root(["ip", "link", "set", "dev", iface, "up"], no_sudo=no_sudo))


def open_can_socket(iface: str) -> socket.socket:
    if not hasattr(socket, "PF_CAN"):
        raise TestError("this Python build has no SocketCAN support.")
    sock = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    try:
        sock.bind((iface,))
    except OSError as exc:
        sock.close()
        raise TestError(f"could not bind to {iface}: {exc}") from exc
    return sock


# ------------------------------------------------------------------ #
# Reporting + run loop                                                 #
# ------------------------------------------------------------------ #

def print_summary(messages: list[Message]) -> None:
    print(f"Parsed {len(messages)} message(s):")
    for msg in messages:
        kind = "ext" if msg.extended else "std"
        id_text = f"0x{msg.can_id:08X}" if msg.extended else f"0x{msg.can_id:03X}"
        print(f"  {msg.name}  id={id_text} ({kind}) dlc={msg.dlc} "
              f"signals={len(msg.signals)}")
        for sig in msg.signals:
            order = "Intel" if sig.little_endian else "Motorola"
            sign = "signed" if sig.signed else "unsigned"
            unit = f" {sig.unit}" if sig.unit else ""
            print(f"      {sig.name}: {sig.start_bit}|{sig.length} {order} {sign} "
                  f"[{sig.minimum:g}..{sig.maximum:g}]{unit}")


def run(iface: str, messages: list[Message], *, rate_hz: float, duration: float) -> None:
    period = 1.0 / rate_hz
    total_rate = rate_hz * len(messages)
    forever = duration <= 0
    print(f"Sending {len(messages)} message(s) on {iface} at {rate_hz:g} Hz each "
          f"({total_rate:g} frames/s total)"
          + ("" if forever else f" for {duration:g}s") + ".")
    print("Connect CANoScope to this interface and load the same DBC. Ctrl+C to stop.")

    sock = open_can_socket(iface)
    start = time.monotonic()
    sent = 0
    tick = 0
    try:
        while forever or (time.monotonic() - start) < duration:
            t = time.monotonic() - start
            for phase, msg in enumerate(messages):
                sock.send(pack_frame(msg, encode_message(msg, t, phase)))
                sent += 1
            tick += 1
            target = start + tick * period
            sleep = target - time.monotonic()
            if sleep > 0:
                time.sleep(sleep)
    except KeyboardInterrupt:
        print()
    finally:
        sock.close()

    elapsed = time.monotonic() - start
    fps = sent / elapsed if elapsed else 0.0
    print(f"Sent {sent} frame(s) in {elapsed:.1f}s ({fps:.0f} frames/s).")


def launch_app(root: Path, iface: str) -> None:
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        raise TestError("no graphical display detected; omit --launch-app.")
    binary = root / "build" / "canoscope"
    if not binary.exists():
        raise TestError(f"binary not found ({binary}); run 'make' first.")
    subprocess.Popen([str(binary)], cwd=str(root))
    print(f"Launched {binary}. In the app: connect to {iface}, then "
          "Signal Analysis Viewer > Load DBC.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate DBC-encoded CAN traffic for testing CANoScope.")
    parser.add_argument("--dbc", default=DEFAULT_DBC,
                        help=f"DBC file to encode from (default: {DEFAULT_DBC})")
    parser.add_argument("--iface", default="vcan0",
                        help="virtual CAN interface (default: vcan0)")
    parser.add_argument("--rate", type=float, default=50.0,
                        help="frames per second for each message (default: 50)")
    parser.add_argument("--duration", type=float, default=60.0,
                        help="seconds to run; 0 = until Ctrl+C (default: 60)")
    parser.add_argument("--list", action="store_true",
                        help="parse the DBC, print its signals, and exit")
    parser.add_argument("--launch-app", action="store_true",
                        help="launch the GTK application before sending traffic")
    parser.add_argument("--no-setup", action="store_true",
                        help="do not create/bring up the vcan interface")
    parser.add_argument("--no-sudo", action="store_true",
                        help="fail instead of using sudo/pkexec for vcan setup")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = Path(__file__).resolve().parents[1]

    dbc_path = Path(args.dbc)
    if not dbc_path.is_absolute():
        dbc_path = (root / dbc_path).resolve()

    try:
        messages = parse_dbc(dbc_path)
        print(f"Loaded {dbc_path}")
        print_summary(messages)
        if args.list:
            return 0

        if args.rate <= 0:
            raise TestError("--rate must be greater than 0")

        ensure_vcan(args.iface, no_setup=args.no_setup, no_sudo=args.no_sudo)
        if args.launch_app:
            launch_app(root, args.iface)
            time.sleep(1.0)
        run(args.iface, messages, rate_hz=args.rate, duration=args.duration)
        return 0
    except TestError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
