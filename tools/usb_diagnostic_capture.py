#!/usr/bin/env python3
"""Timestamp Ciallo's USB kernel events, operator marks, and read-only esptool output."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import threading
import time
from datetime import datetime
from zoneinfo import ZoneInfo


ZONE = ZoneInfo("Asia/Shanghai")
MARK_TAG = "SATORI_USB_MARK"
DEVICE_PORT = "/dev/ttyACM0"


def clock_fields(epoch_us=None, monotonic_us=None):
    now_ns = time.time_ns()
    us = int(epoch_us) if epoch_us else now_ns // 1000
    return {
        "time": datetime.fromtimestamp(us / 1_000_000, ZONE).isoformat(
            timespec="microseconds"
        ),
        "epoch_us": us,
        "monotonic_us": int(monotonic_us) if monotonic_us else time.monotonic_ns() // 1000,
    }


class Recorder:
    def __init__(self, directory):
        directory.mkdir(parents=True, exist_ok=False, mode=0o700)
        self.directory = directory
        self.lock = threading.Lock()
        self.events = (directory / "events.jsonl").open("w", encoding="utf-8")
        self.timeline = (directory / "timeline.txt").open("w", encoding="utf-8")

    def write(self, source, message, epoch_us=None, monotonic_us=None):
        record = {**clock_fields(epoch_us, monotonic_us), "source": source, "message": message}
        with self.lock:
            self.events.write(json.dumps(record, ensure_ascii=False) + "\n")
            self.events.flush()
            self.timeline.write(f"{record['time']} [{source}] {message}\n")
            self.timeline.flush()

    def close(self):
        self.events.close()
        self.timeline.close()


def watch_journal(recorder, process):
    for line in process.stdout:
        try:
            entry = json.loads(line)
        except json.JSONDecodeError:
            continue
        message = entry.get("MESSAGE", "")
        if not isinstance(message, str):
            continue
        is_mark = entry.get("SYSLOG_IDENTIFIER") == MARK_TAG
        is_kernel = entry.get("_TRANSPORT") == "kernel"
        if is_mark or is_kernel:
            recorder.write(
                "manual" if is_mark else "kernel",
                message,
                entry.get("_REALTIME_TIMESTAMP"),
                entry.get("_MONOTONIC_TIMESTAMP"),
            )


def run_esptool(recorder, args):
    command = "esptool --chip esp32c3 --port " + shlex.quote(DEVICE_PORT)
    command += " --baud 115200 " + " ".join(shlex.quote(arg) for arg in args)
    recorder.write("phase", f"esptool start: {command}")
    process = subprocess.Popen(
        ["sg", "dialout", "-c", command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    for line in process.stdout:
        recorder.write("esptool", line.rstrip("\r\n"))
    status = process.wait()
    recorder.write("phase", f"esptool exit: {status}")
    return status


def capture(args):
    output = Path(args.output).expanduser().resolve()
    recorder = Recorder(output)
    journal = subprocess.Popen(
        ["journalctl", "--follow", "--lines=0", "--output=json", "--no-pager"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    reader = threading.Thread(target=watch_journal, args=(recorder, journal), daemon=True)
    reader.start()
    status = 0
    try:
        time.sleep(1)  # Let journalctl subscribe before marking or probing.
        recorder.write("phase", "capture started")
        recorder.write("phase", f"idle observation: {args.idle_seconds}s")
        time.sleep(args.idle_seconds)
        if args.probe or args.rom_probe or args.rom_backup:
            if not Path(DEVICE_PORT).exists():
                recorder.write("phase", f"probe skipped: {DEVICE_PORT} absent")
                status = 2
            elif args.rom_probe:
                readback = output / "factory-first-sector-readback.bin"
                status = run_esptool(
                    recorder,
                    [
                        "--no-stub", "--after", "no-reset", "read-flash",
                        "--no-progress", "0x10000", "0x1000", str(readback),
                    ],
                )
                if readback.exists():
                    os.chmod(readback, 0o600)
            elif args.rom_backup:
                backup = output / "factory-partition-backup.bin"
                status = run_esptool(
                    recorder,
                    [
                        "--no-stub", "--before", "no-reset", "--after", "no-reset",
                        "read-flash", "--no-progress", "0x10000", "0x200000",
                        str(backup),
                    ],
                )
                if backup.exists():
                    os.chmod(backup, 0o600)
            else:
                status = run_esptool(recorder, ["--after", "no-reset", "flash-id"])
                if status == 0:
                    readback = output / "config-sector-readback.bin"
                    status = run_esptool(
                        recorder,
                        [
                            "--before", "no-reset", "--after", "no-reset",
                            "read-flash", "--no-progress", "0x300000", "0x1000",
                            str(readback),
                        ],
                    )
                    if readback.exists():
                        os.chmod(readback, 0o600)
        recorder.write("phase", f"post-observation: {args.after_seconds}s")
        time.sleep(args.after_seconds)
        recorder.write("phase", f"capture finished: status {status}")
    finally:
        journal.terminate()
        try:
            journal.wait(timeout=3)
        except subprocess.TimeoutExpired:
            journal.kill()
            journal.wait()
        reader.join(timeout=3)
        recorder.close()
    print(output / "timeline.txt")
    return status


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    capture_parser = sub.add_parser("capture", help="record USB and optionally probe flash read")
    capture_parser.add_argument("--output", required=True, help="new private output directory")
    capture_parser.add_argument("--idle-seconds", type=int, default=30)
    capture_parser.add_argument("--after-seconds", type=int, default=15)
    probes = capture_parser.add_mutually_exclusive_group()
    probes.add_argument("--probe", action="store_true", help="run read-only esptool checks")
    probes.add_argument(
        "--rom-probe", action="store_true",
        help="read factory's first sector using ROM without uploading a stub",
    )
    probes.add_argument(
        "--rom-backup", action="store_true",
        help="read the 2 MiB factory partition using ROM without a stub",
    )
    mark_parser = sub.add_parser("mark", help="timestamp a manual unplug/reset event")
    mark_parser.add_argument("message", help="e.g. 'unplug complete' or 'RESET pressed'")
    args = parser.parse_args()
    if args.action == "mark":
        event_time = clock_fields()["time"]
        subprocess.run(
            ["logger", "-t", MARK_TAG, "--", f"operator_time={event_time} {args.message}"],
            check=True,
        )
        print(event_time, args.message)
        return
    if args.idle_seconds < 0 or args.after_seconds < 0:
        parser.error("observation times must be nonnegative")
    raise SystemExit(capture(args))


if __name__ == "__main__":
    main()
