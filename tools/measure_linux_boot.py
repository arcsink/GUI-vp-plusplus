#!/usr/bin/env python3

import argparse
import csv
import os
import re
import selectors
import signal
import subprocess
import sys
import time
from pathlib import Path


LOGIN_PATTERNS = (
    re.compile(r"Welcome to Buildroot"),
    re.compile(r"buildroot login:"),
)


def parse_size(value: str) -> int:
    text = value.strip().lower()
    match = re.fullmatch(r"(\d+)([kmgt]?i?b?)?", text)
    if not match:
        raise argparse.ArgumentTypeError(f"invalid size: {value}")

    number = int(match.group(1))
    suffix = match.group(2) or ""
    scale = {
        "": 1,
        "b": 1,
        "k": 1024,
        "kb": 1024,
        "kib": 1024,
        "m": 1024**2,
        "mb": 1024**2,
        "mib": 1024**2,
        "g": 1024**3,
        "gb": 1024**3,
        "gib": 1024**3,
        "t": 1024**4,
        "tb": 1024**4,
        "tib": 1024**4,
    }
    if suffix not in scale:
        raise argparse.ArgumentTypeError(f"invalid size suffix in: {value}")
    return number * scale[suffix]


def parse_int_auto(value: str) -> int:
    text = value.strip().lower()
    try:
        return int(text, 0)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer: {value}") from exc


def fmt_size(value: int) -> str:
    units = (
        (1024**3, "GiB"),
        (1024**2, "MiB"),
        (1024, "KiB"),
    )
    for factor, suffix in units:
        if value >= factor and value % factor == 0:
            return f"{value // factor}{suffix}"
    return str(value)


def build_command(repo_root: Path, args: argparse.Namespace, window_size: int) -> list[str]:
    command = [
        str(repo_root / "riscv-vp-plusplus/vp/build/bin/linux64-mc-vp"),
        "--use-data-dmi",
        "--tlm-global-quantum=1000000",
        "--use-dbbcache",
        "--use-lscache",
        f"--tun-device={args.tun_device}",
        f"--dtb-file={repo_root / 'dt/linux-vp_rv64_mc.dtb'}",
        f"--kernel-file={repo_root / 'buildroot_rv64/output/images/Image'}",
        f"--mram-root-image={repo_root / 'runtime_mram/mram_rv64_root.img'}",
        f"--mram-data-image={repo_root / 'runtime_mram/mram_rv64_data.img'}",
        f"--memory-size={2 * 1024**3}",
        f"--cache-ace-dram-window-size={window_size}",
        str(repo_root / "buildroot_rv64/output/images/fw_jump.elf"),
    ]
    if args.window_start is not None:
        command.insert(-2, f"--cache-ace-dram-window-start={args.window_start}")
    return command


def run_once(repo_root: Path, args: argparse.Namespace, window_size: int) -> dict[str, str]:
    command = build_command(repo_root, args, window_size)
    env = os.environ.copy()
    env.setdefault("TERM", "dumb")

    start = time.monotonic()
    proc = subprocess.Popen(
        command,
        cwd=repo_root,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
        env=env,
        preexec_fn=os.setsid,
    )

    selector = selectors.DefaultSelector()
    assert proc.stdout is not None
    selector.register(proc.stdout, selectors.EVENT_READ)

    status = "timeout"
    boot_seconds = ""
    matched_line = ""
    log_lines: list[str] = []

    try:
        while True:
            now = time.monotonic()
            remaining = args.timeout - (now - start)
            if remaining <= 0:
                break

            events = selector.select(timeout=min(0.5, remaining))
            if not events:
                if proc.poll() is not None:
                    status = f"exited:{proc.returncode}"
                    break
                continue

            for key, _ in events:
                line = key.fileobj.readline()
                if not line:
                    if proc.poll() is not None:
                        status = f"exited:{proc.returncode}"
                        break
                    continue
                log_lines.append(line.rstrip("\n"))
                if args.verbose:
                    sys.stdout.write(line)
                    sys.stdout.flush()
                if any(pattern.search(line) for pattern in LOGIN_PATTERNS):
                    status = "booted"
                    boot_seconds = f"{time.monotonic() - start:.3f}"
                    matched_line = line.rstrip("\n")
                    raise StopIteration
            else:
                continue
            break
    except StopIteration:
        pass
    finally:
        selector.close()
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait(timeout=5)

    last_line = next((line for line in reversed(log_lines) if line.strip()), "")
    return {
        "window_size_bytes": str(window_size),
        "window_size_human": fmt_size(window_size),
        "status": status,
        "boot_seconds": boot_seconds,
        "matched_line": matched_line,
        "last_output": last_line,
        "command": " ".join(command),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Measure linux64-mc-vp boot time for different cache ACE DRAM window sizes."
    )
    parser.add_argument(
        "--window-size",
        dest="window_sizes",
        action="append",
        type=parse_size,
        required=True,
        help="cache ACE DRAM window size, e.g. 64KiB, 1MiB, 16MiB",
    )
    parser.add_argument(
        "--window-start",
        type=parse_int_auto,
        default=None,
        help="cache ACE DRAM window start address",
    )
    parser.add_argument("--timeout", type=int, default=180, help="per-run timeout in seconds")
    parser.add_argument("--tun-device", default="tun10", help="SLIP backend TUN device name")
    parser.add_argument(
        "--output",
        default="boot_measurements/linux64_mc_cache_ace_boot.csv",
        help="CSV output path relative to repo root",
    )
    parser.add_argument("--verbose", action="store_true", help="stream VP output while measuring")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    required_paths = [
        repo_root / "riscv-vp-plusplus/vp/build/bin/linux64-mc-vp",
        repo_root / "dt/linux-vp_rv64_mc.dtb",
        repo_root / "buildroot_rv64/output/images/Image",
        repo_root / "buildroot_rv64/output/images/fw_jump.elf",
        repo_root / "runtime_mram/mram_rv64_root.img",
        repo_root / "runtime_mram/mram_rv64_data.img",
    ]
    missing = [str(path) for path in required_paths if not path.exists()]
    if missing:
        for path in missing:
            print(f"missing required file: {path}", file=sys.stderr)
        return 2

    output_path = repo_root / args.output
    output_path.parent.mkdir(parents=True, exist_ok=True)

    rows = []
    for window_size in args.window_sizes:
        print(f"[measure] cache-ace window {fmt_size(window_size)}")
        row = run_once(repo_root, args, window_size)
        rows.append(row)
        print(
            f"[result] status={row['status']} window={row['window_size_human']} boot_seconds={row['boot_seconds'] or '-'}"
        )

    with output_path.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=[
                "window_size_bytes",
                "window_size_human",
                "status",
                "boot_seconds",
                "matched_line",
                "last_output",
                "command",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)

    print(f"[saved] {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
