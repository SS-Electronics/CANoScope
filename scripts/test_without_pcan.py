#!/usr/bin/env python3
"""Local no-hardware smoke test for CANoScope.

This script uses Linux SocketCAN with a virtual CAN interface (`vcan0` by
default), so no PEAK/PCAN hardware is required.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path


CAN_FRAME_FMT = "=IB3x8s"
CAN_FRAME_SIZE = struct.calcsize(CAN_FRAME_FMT)
CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_ERR_FLAG = 0x20000000
CAN_SFF_MASK = 0x7FF
CAN_EFF_MASK = 0x1FFFFFFF
DEFAULT_TEST_ID = 0x123
DEFAULT_TEST_DATA = bytes.fromhex("DE AD BE EF")


@dataclass(frozen=True)
class CanFrame:
    can_id: int
    data: bytes
    is_extended: bool = False
    is_remote: bool = False
    is_error: bool = False


DEMO_TRAFFIC_FRAMES = (
    CanFrame(0x123, bytes.fromhex("DE AD BE EF")),
    CanFrame(0x124, bytes.fromhex("01 23 45 67 89 AB CD EF")),
    CanFrame(0x125, bytes.fromhex("AA 55 00 FF")),
)


class SmokeError(RuntimeError):
    """Raised for expected smoke-test failures."""


def project_root() -> Path:
    return Path(__file__).resolve().parents[1]


def run_cmd(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    check: bool = True,
    quiet: bool = False,
) -> subprocess.CompletedProcess[str]:
    kwargs = {
        "cwd": str(cwd) if cwd else None,
        "text": True,
    }
    if quiet:
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.PIPE

    proc = subprocess.run(cmd, **kwargs)
    if check and proc.returncode != 0:
        raise SmokeError(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return proc


def root_cmd(cmd: list[str], *, no_sudo: bool) -> list[str]:
    if os.geteuid() == 0:
        return cmd

    if no_sudo:
        raise SmokeError(
            "vcan setup needs root privileges. Re-run without --no-sudo, "
            "run with sudo, or create vcan0 manually."
        )

    sudo = shutil.which("sudo")
    if not sudo:
        raise SmokeError("sudo was not found; create the vcan interface manually.")

    return [sudo, *cmd]


def require_command(name: str) -> None:
    if not shutil.which(name):
        raise SmokeError(f"required command not found: {name}")


def interface_exists(iface: str) -> bool:
    proc = run_cmd(
        ["ip", "link", "show", "dev", iface],
        check=False,
        quiet=True,
    )
    return proc.returncode == 0


def ensure_can_interface_type(iface: str) -> None:
    type_path = Path("/sys/class/net") / iface / "type"
    try:
        iftype = type_path.read_text(encoding="ascii").strip()
    except OSError as exc:
        raise SmokeError(f"could not inspect interface {iface}: {exc}") from exc

    if iftype != "280":
        raise SmokeError(
            f"{iface} exists but is not a CAN interface "
            f"(kernel type {iftype}, expected 280)."
        )


def ensure_vcan(iface: str, *, no_sudo: bool, no_setup: bool) -> None:
    require_command("ip")

    if no_setup:
        if not interface_exists(iface):
            raise SmokeError(f"{iface} does not exist and --no-setup was used.")
        ensure_can_interface_type(iface)
        return

    if not interface_exists(iface):
        if shutil.which("modprobe"):
            run_cmd(root_cmd(["modprobe", "vcan"], no_sudo=no_sudo), check=False)
        run_cmd(root_cmd(["ip", "link", "add", "dev", iface, "type", "vcan"],
                         no_sudo=no_sudo))

    ensure_can_interface_type(iface)
    run_cmd(root_cmd(["ip", "link", "set", "dev", iface, "up"],
                     no_sudo=no_sudo))


def pack_can_frame(
    can_id: int,
    data: bytes,
    *,
    is_extended: bool = False,
    is_remote: bool = False,
) -> bytes:
    max_id = CAN_EFF_MASK if is_extended else CAN_SFF_MASK
    if not (0 <= can_id <= max_id):
        kind = "extended" if is_extended else "standard"
        raise ValueError(f"{kind} CAN ID must be in range 0x000..0x{max_id:X}")
    if len(data) > 8:
        raise ValueError("classic CAN payload cannot exceed 8 bytes")

    raw_id = can_id
    if is_extended:
        raw_id |= CAN_EFF_FLAG
    if is_remote:
        raw_id |= CAN_RTR_FLAG
    return struct.pack(CAN_FRAME_FMT, raw_id, len(data), data.ljust(8, b"\x00"))


def pack_demo_frame(frame: CanFrame) -> bytes:
    return pack_can_frame(
        frame.can_id,
        frame.data,
        is_extended=frame.is_extended,
        is_remote=frame.is_remote,
    )


def unpack_can_frame(raw: bytes) -> CanFrame:
    raw_id, dlc, data = struct.unpack(CAN_FRAME_FMT, raw[:CAN_FRAME_SIZE])
    is_extended = bool(raw_id & CAN_EFF_FLAG)
    is_remote = bool(raw_id & CAN_RTR_FLAG)
    is_error = bool(raw_id & CAN_ERR_FLAG)
    mask = CAN_EFF_MASK if is_extended else CAN_SFF_MASK
    return CanFrame(
        raw_id & mask,
        data[:dlc],
        is_extended=is_extended,
        is_remote=is_remote,
        is_error=is_error,
    )


def frame_signature(frame: CanFrame) -> tuple[int, bool, bool, bool, bytes]:
    return (
        frame.can_id,
        frame.is_extended,
        frame.is_remote,
        frame.is_error,
        frame.data,
    )


def format_can_frame(frame: CanFrame) -> str:
    id_text = f"{frame.can_id:08X}" if frame.is_extended else f"{frame.can_id:03X}"
    type_bits = ["Ext" if frame.is_extended else "Std"]
    if frame.is_remote:
        type_bits.append("RTR")
    if frame.is_error:
        type_bits.append("Err")

    data_text = "-" if not frame.data else frame.data.hex(" ").upper()
    return (
        f"id={id_text} type={'+'.join(type_bits)} "
        f"dlc={len(frame.data)} data={data_text}"
    )


def open_can_socket(iface: str) -> socket.socket:
    if not hasattr(socket, "PF_CAN"):
        raise SmokeError("this Python build does not expose SocketCAN support")

    sock = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.bind((iface,))
    return sock


def socketcan_loopback_test(iface: str, timeout_s: float) -> None:
    frame = pack_can_frame(DEFAULT_TEST_ID, DEFAULT_TEST_DATA)

    with open_can_socket(iface) as rx, open_can_socket(iface) as tx:
        rx.setblocking(False)
        tx.send(frame)

        ready, _, _ = select.select([rx], [], [], timeout_s)
        if not ready:
            raise SmokeError(f"timed out waiting for loopback frame on {iface}")

        received = unpack_can_frame(rx.recv(CAN_FRAME_SIZE))
        if received.can_id != DEFAULT_TEST_ID or received.data != DEFAULT_TEST_DATA:
            raise SmokeError(
                "received unexpected loopback frame: "
                f"{format_can_frame(received)}"
            )


def build_app(root: Path) -> Path:
    run_cmd(["make"], cwd=root)
    binary = root / "build" / "canoscope"
    if not binary.exists():
        raise SmokeError(f"build finished but binary is missing: {binary}")
    return binary


def drain_received_frames(
    rx: socket.socket,
    *,
    own_signatures: set[tuple[int, bool, bool, bool, bytes]],
    start_time: float,
    print_all_bus: bool,
) -> int:
    printed = 0

    while True:
        ready, _, _ = select.select([rx], [], [], 0)
        if not ready:
            return printed

        frame = unpack_can_frame(rx.recv(CAN_FRAME_SIZE))
        elapsed = time.monotonic() - start_time
        own_frame = frame_signature(frame) in own_signatures

        if own_frame and not print_all_bus:
            continue

        source = "test traffic" if own_frame else "CANoScope/external"
        print(f"[{elapsed:8.3f}s] {source}: {format_can_frame(frame)}",
              flush=True)
        printed += 1


def traffic_interval_from_args(args: argparse.Namespace) -> tuple[float, float]:
    if args.traffic_rate is not None:
        if args.traffic_rate <= 0:
            raise SmokeError("--traffic-rate must be greater than 0")
        return (
            1.0 / (args.traffic_rate * len(DEMO_TRAFFIC_FRAMES)),
            args.traffic_rate,
        )

    if args.traffic_period <= 0:
        raise SmokeError("--traffic-period must be greater than 0")
    return (
        args.traffic_period,
        1.0 / (args.traffic_period * len(DEMO_TRAFFIC_FRAMES)),
    )


def run_traffic_session(
    iface: str,
    *,
    traffic_seconds: float,
    listen_seconds: float,
    traffic_interval_s: float,
    per_message_rate_hz: float,
    print_all_bus: bool,
) -> None:
    if traffic_seconds < 0:
        raise SmokeError("--traffic-seconds cannot be negative")
    if listen_seconds < 0:
        raise SmokeError("--listen-seconds cannot be negative")

    total_seconds = max(traffic_seconds, listen_seconds)
    if total_seconds <= 0:
        return

    own_signatures = {frame_signature(frame) for frame in DEMO_TRAFFIC_FRAMES}
    start = time.monotonic()
    deadline = start + total_seconds
    traffic_deadline = start + traffic_seconds
    next_send = start
    sent_count = 0
    rx_print_count = 0
    frame_index = 0

    print(f"Listening on {iface} for {total_seconds:.1f}s.")
    if traffic_seconds > 0:
        total_rate = per_message_rate_hz * len(DEMO_TRAFFIC_FRAMES)
        print(
            "Sending 3 demo messages "
            f"at {per_message_rate_hz:.3g} Hz each "
            f"({total_rate:.3g} total frames/s) for {traffic_seconds:.1f}s."
        )
        for frame in DEMO_TRAFFIC_FRAMES:
            print(f"  demo: {format_can_frame(frame)}")
    print("CANoScope/external frames will be printed as they arrive.")
    if traffic_seconds > 0 and not print_all_bus:
        print(
            "Generated demo loopback frames are hidden; "
            "add --print-all-bus to show them."
        )

    with open_can_socket(iface) as rx:
        rx.setblocking(False)
        tx = open_can_socket(iface) if traffic_seconds > 0 else None
        try:
            while time.monotonic() < deadline:
                now = time.monotonic()

                if tx and now < traffic_deadline and now >= next_send:
                    frame = DEMO_TRAFFIC_FRAMES[frame_index]
                    tx.send(pack_demo_frame(frame))
                    sent_count += 1
                    frame_index = (frame_index + 1) % len(DEMO_TRAFFIC_FRAMES)
                    next_send += traffic_interval_s
                    if next_send <= now:
                        next_send = now + traffic_interval_s

                rx_print_count += drain_received_frames(
                    rx,
                    own_signatures=own_signatures,
                    start_time=start,
                    print_all_bus=print_all_bus,
                )

                now = time.monotonic()
                next_wakeup = deadline
                if tx and now < traffic_deadline:
                    next_wakeup = min(next_wakeup, next_send)
                timeout = max(0.0, min(0.05, next_wakeup - now))
                if timeout > 0:
                    ready, _, _ = select.select([rx], [], [], timeout)
                    if ready:
                        rx_print_count += drain_received_frames(
                            rx,
                            own_signatures=own_signatures,
                            start_time=start,
                            print_all_bus=print_all_bus,
                        )
        finally:
            if tx:
                tx.close()

    print(
        f"Session complete: sent {sent_count} demo frame(s), "
        f"printed {rx_print_count} received frame(s)."
    )


def send_demo_traffic(iface: str, seconds: float, period_s: float) -> None:
    """Compatibility wrapper for older callers."""
    per_message_rate = 1.0 / (period_s * len(DEMO_TRAFFIC_FRAMES))
    run_traffic_session(
        iface,
        traffic_seconds=seconds,
        listen_seconds=seconds,
        traffic_interval_s=period_s,
        per_message_rate_hz=per_message_rate,
        print_all_bus=False,
    )


def launch_app(binary: Path) -> None:
    display = os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
    if not display:
        raise SmokeError(
            "no graphical display was detected; omit --launch-app or run "
            "from a desktop session."
        )

    subprocess.Popen([str(binary)], cwd=str(binary.parent.parent))
    print(f"Launched {binary}. In the app, connect to vcan0.")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build and test CANoScope locally using vcan.",
    )
    parser.add_argument(
        "--iface",
        default="vcan0",
        help="virtual CAN interface to use (default: vcan0)",
    )
    parser.add_argument(
        "--skip-build",
        action="store_true",
        help="skip running make before the smoke test",
    )
    parser.add_argument(
        "--no-setup",
        action="store_true",
        help="do not create or bring up the vcan interface",
    )
    parser.add_argument(
        "--no-sudo",
        action="store_true",
        help="fail instead of using sudo for vcan setup",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=2.0,
        help="SocketCAN loopback receive timeout in seconds",
    )
    parser.add_argument(
        "--traffic-seconds",
        type=float,
        default=0.0,
        help="send demo CAN traffic for this many seconds after the smoke test",
    )
    parser.add_argument(
        "--traffic-rate",
        type=float,
        default=None,
        help=(
            "target repeat rate in Hz for each of the 3 demo messages; "
            "overrides --traffic-period"
        ),
    )
    parser.add_argument(
        "--traffic-period",
        type=float,
        default=0.25,
        help="seconds between generated demo frames when --traffic-rate is not set",
    )
    parser.add_argument(
        "--listen-seconds",
        type=float,
        default=0.0,
        help=(
            "listen and print CANoScope/external CAN frames for this many "
            "seconds"
        ),
    )
    parser.add_argument(
        "--print-all-bus",
        action="store_true",
        help="also print the script's own generated demo frames",
    )
    parser.add_argument(
        "--launch-app",
        action="store_true",
        help="launch the GTK application after the smoke test",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = project_root()

    try:
        binary = root / "build" / "canoscope"
        if not args.skip_build:
            print("Building CANoScope...")
            binary = build_app(root)

        print(f"Preparing {args.iface}...")
        ensure_vcan(args.iface, no_sudo=args.no_sudo, no_setup=args.no_setup)

        print("Running SocketCAN loopback smoke test...")
        socketcan_loopback_test(args.iface, args.timeout)
        print("Loopback OK: vcan is ready for local PCAN-free testing.")

        if args.launch_app:
            launch_app(binary)

        if args.traffic_seconds > 0 or args.listen_seconds > 0:
            traffic_interval_s, per_message_rate_hz = traffic_interval_from_args(args)
            run_traffic_session(
                args.iface,
                traffic_seconds=args.traffic_seconds,
                listen_seconds=args.listen_seconds,
                traffic_interval_s=traffic_interval_s,
                per_message_rate_hz=per_message_rate_hz,
                print_all_bus=args.print_all_bus,
            )

        print("")
        print("Next:")
        print(f"  1. Run: {binary}")
        print(f"  2. File > Connect > select {args.iface}")
        print("  3. To generate 3 visible Rx rows at 10 Hz each:")
        print(
            "     python3 scripts/test_without_pcan.py "
            "--skip-build --no-setup --traffic-seconds 30 --traffic-rate 10"
        )
        print("  4. To print what CANoScope transmits:")
        print(
            "     python3 scripts/test_without_pcan.py "
            "--skip-build --no-setup --listen-seconds 30"
        )
        return 0

    except SmokeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
