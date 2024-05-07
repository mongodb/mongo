#!/usr/bin/env python3

from dataclasses import dataclass
import argparse
from threading import Event
import json
import signal
import subprocess
import sys
from profilerlib import CallMetrics

stop_event = Event()


def on_stop_signal(signum, frame):
    stop_event.set()


def get_profiler_stats():
    r = subprocess.check_output([
        'mongosh', '--quiet', '--eval',
        'print(EJSON.stringify(db.runCommand({serverStatus: 1}).tracing_profiler))'
    ])
    return json.loads(r)


def get_call_metrics():
    return CallMetrics.from_json(get_profiler_stats())


def main():
    signal.signal(signal.SIGINT, on_stop_signal)

    parser = argparse.ArgumentParser(
        usage="usage: %(prog)s [options]", description="Collects a profiler data from mongod",
        formatter_class=argparse.RawTextHelpFormatter, epilog="Press CTRL-C to stop profiling")

    parser.add_argument("-s", "--sleep", dest="sleep", default=None,
                        help="profiles for given number of seconds, or indefinitely if not")

    args = parser.parse_args()

    before_metrics = get_call_metrics()

    sleep_sec = None
    if args.sleep is not None:
        sleep_sec = int(args.sleep)

    if sleep_sec is None:
        print("Profiling indefinitely until interrupted...", file=sys.stderr)
    else:
        print(f"Profiling for {sleep_sec} seconds...", file=sys.stderr)

    print("Press CTRL-C to stop profiling", file=sys.stderr)
    stop_event.wait(sleep_sec)

    after_metrics = get_call_metrics()

    after_metrics.subtract(before_metrics)
    print(after_metrics.to_json())


if __name__ == "__main__":
    main()
