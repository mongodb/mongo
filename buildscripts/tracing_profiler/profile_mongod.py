#!/usr/bin/env python3

import argparse
import json
import signal
import subprocess
import sys
from threading import Event

from profilerlib import CallMetrics


def get_profiler_stats(args):
    command = [
        "mongosh",
        "--quiet",
        "--eval",
        "print(EJSON.stringify(db.runCommand({serverStatus: 1}).tracing_profiler))",
    ]

    if args.username:
        command.extend(["--username", args.username])
    if args.password:
        command.extend(["--password", args.password])
    if args.tls:
        command.append("--tls")
    if args.tlsAllowInvalidCertificates:
        command.append("--tlsAllowInvalidCertificates")
    if args.tlsAllowInvalidHostnames:
        command.append("--tlsAllowInvalidHostnames")

    r = subprocess.check_output(command)
    return json.loads(r)


def get_call_metrics(args):
    return CallMetrics.from_json(get_profiler_stats(args))


def main():
    stop_event = Event()
    signal.signal(signal.SIGINT, lambda *_: stop_event.set())

    parser = argparse.ArgumentParser(
        usage="usage: %(prog)s [options]",
        description="Collects a profiler data from mongod",
        formatter_class=argparse.RawTextHelpFormatter,
        epilog="Press CTRL-C to stop profiling",
    )
    parser.add_argument(
        "-s",
        "--sleep",
        dest="sleep",
        type=int,
        help="profiles for given number of seconds, or indefinitely if not",
    )
    # The following are args that get passed through to mongosh
    # See https://github.com/mongodb-js/mongosh?tab=readme-ov-file#cli-usage
    parser.add_argument(
        "-u",
        "--username",
        dest="username",
        help="Username for authentication",
    )
    parser.add_argument(
        "-p",
        "--password",
        dest="password",
        help="Password for authentication",
    )
    parser.add_argument(
        "--tls", dest="tls", action="store_true", help="Use TLS for all connections"
    )
    parser.add_argument(
        "--tlsAllowInvalidCertificates",
        dest="tlsAllowInvalidCertificates",
        action="store_true",
        help="Allow connections to servers with invalid certificates",
    )
    parser.add_argument(
        "--tlsAllowInvalidHostnames",
        dest="tlsAllowInvalidHostnames",
        action="store_true",
        help="Allow connections to servers with non-matching hostnames",
    )

    args = parser.parse_args()

    before_metrics = get_call_metrics(args)

    sleep_sec = None
    if args.sleep is not None:
        sleep_sec = args.sleep

    if sleep_sec is None:
        print("Profiling indefinitely until interrupted...", file=sys.stderr)
    else:
        print(f"Profiling for {sleep_sec} seconds...", file=sys.stderr)

    print("Press CTRL-C to stop profiling", file=sys.stderr)
    stop_event.wait(sleep_sec)

    after_metrics = get_call_metrics(args)

    after_metrics.subtract(before_metrics)
    print(after_metrics.to_json())


if __name__ == "__main__":
    main()
