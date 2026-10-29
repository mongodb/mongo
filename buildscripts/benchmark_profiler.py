#!/usr/bin/env python3
"""
On-CPU flamegraph profiling for a whole benchmark task, using `perf record`. Currently this only supports benchmarks_sep.

Invoked from Evergreen as two steps: `start-task` (before the benchmark suite) starts a detached,
system-wide recording (`perf record -e instructions:u -a -g -F max`) so every benchmark child process is captured
and `process-task` (after) stops it and turns the perf.data into a flamegraph SVG and a bundle tarball.
Both are gated on the run's `enable_linux_perf` param, so they no-op on ordinary (non-profiling) runs.
"""

import argparse
import gzip
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import tempfile
import time

FLAMEGRAPH_REPO_URL = "https://github.com/mongodb-forks/flamegraph.git"
_FLAMEGRAPH_SCRIPTS = ("flamegraph.pl", "stackcollapse-perf.pl")

PERF_DATA_FILENAME = "perf.data"
PROFILING_LINKS_FILENAME = "profiling_links.json"
# Currently on-CPU profiling is only supported on the benchmarks_sep task.
TASK_NAME = "benchmarks_sep"


def build_perf_record_cmd(*, perf_data: str) -> list[str]:
    """Build a system-wide `perf record` command for whole-task profiling.

    Records the whole machine (`-a`) for the duration of an externally-run workload (e.g. the resmoke
    benchmark suite), so every benchmark child process is captured. Sampling is weighted by the
    `instructions:u` event (user-space instructions only), matching the benchmarks'
    `instructions_per_iteration` metric (which excludes kernel instructions).
    """
    return [
        "sudo",
        "perf",
        "record",
        "-e",
        "instructions:u",
        "-a",
        "-g",
        "--buildid-all",
        "-F",
        "max",
        "-o",
        perf_data,
    ]


def _run_to_file(argv: list[str], output_path: str) -> None:
    """Run `argv`, writing its stdout to `output_path`. Raises on non-zero exit."""
    with open(output_path, "wb") as out:
        subprocess.run(argv, stdout=out, check=True)


def _missing_flamegraph_scripts(dir_path: str) -> list[str]:
    """Return the required FlameGraph script names not present in `dir_path` (empty list if complete)."""
    return [s for s in _FLAMEGRAPH_SCRIPTS if not os.path.isfile(os.path.join(dir_path, s))]


def ensure_flamegraph_dir() -> str:
    """Return a directory holding the FlameGraph scripts, cloning the MongoDB fork if needed."""
    cache_dir = os.path.join(tempfile.gettempdir(), "mongo-flamegraph")
    if not os.path.isfile(os.path.join(cache_dir, "flamegraph.pl")):
        subprocess.run(["git", "clone", "--depth", "1", FLAMEGRAPH_REPO_URL, cache_dir], check=True)
    missing = _missing_flamegraph_scripts(cache_dir)
    if missing:
        raise FileNotFoundError(
            f"FlameGraph cache {cache_dir} is missing required script(s): {', '.join(missing)}"
        )
    return cache_dir


# Only keep folded stacks whose first frame is a benchmark process (``*_bm``); this excludes system
# noise (mandb, systemd, etc.) captured by the system-wide recording.
_BENCHMARK_RE = re.compile(r"^[a-zA-Z_0-9]+_bm;")


def _merge_folded_files(folded_parts: list[str], merged_path: str) -> None:
    """Merge multiple folded-stack files by summing counts for identical stacks.

    Only stacks from benchmark processes (``*_bm``) are kept; system noise captured by the
    system-wide recording is filtered out. Ideally, we perf record the benchmark binary
    directly rather than the whole system but that requires a more complex benchmark
    setup. As a first iteration, we filter out non-benchmark stacks here.
    """
    totals: dict[str, int] = {}
    for path in folded_parts:
        if not os.path.isfile(path) or os.path.getsize(path) == 0:
            continue
        with open(path) as f:
            for line in f:
                stack, _, count_str = line.rpartition(" ")
                if stack and _BENCHMARK_RE.match(stack):
                    totals[stack] = totals.get(stack, 0) + int(count_str)
    with open(merged_path, "w") as f:
        for stack, count in totals.items():
            f.write(f"{stack} {count}\n")


def render_flamegraph(
    *,
    perf_data: str,
    flamegraph_dir: str,
    output_prefix: str,
    title: str,
) -> str:
    """Post-process a perf.data file into a flamegraph SVG.

    Runs one pipeline per CPU (``perf script -C <cpu> | perl merge | stackcollapse-perf.pl``) so
    both the perf-to-text conversion and the stack folding are parallelised, avoiding a
    single-threaded bottleneck over a large perf.data. Per-connection thread names are collapsed
    inline via perl. The per-CPU folded outputs are merged and rendered into the final SVG.
    """
    folded = output_prefix + ".folded"
    svg = output_prefix + ".svg"

    ncpu = min(os.cpu_count() or 1, 16)
    collapse = os.path.join(flamegraph_dir, "stackcollapse-perf.pl")
    # Merge per-connection thread names inline (mirrors _merge_connection_line).
    merge_expr = r"s/conn\d+/conn/g; s/Shardin\.[a-zA-Z]+-\d+/Shardin.Fixed/g"

    procs: list[subprocess.Popen] = []
    folded_parts: list[str] = []
    for cpu in range(ncpu):
        part = f"{folded}.cpu{cpu}"
        folded_parts.append(part)
        cmd = (
            f"perf script -i {shlex.quote(perf_data)} -C {cpu} "
            f"| perl -pe {shlex.quote(merge_expr)} "
            f"| {shlex.quote(collapse)} --kernel --mongo > {shlex.quote(part)}"
        )
        procs.append(subprocess.Popen(cmd, shell=True))

    for p in procs:
        p.wait()

    _merge_folded_files(folded_parts, folded)
    for part in folded_parts:
        if os.path.isfile(part):
            os.remove(part)

    _run_to_file([os.path.join(flamegraph_dir, "flamegraph.pl"), "--title", title, folded], svg)

    return svg


def _perf_is_running() -> bool:
    """True while the `perf` recorder process is still alive (`pgrep -x`)."""
    return (
        subprocess.run(
            ["pgrep", "-x", "perf"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode
        == 0
    )


def _wait_until_perf_stops(*, timeout_secs: float, poll_secs: float) -> None:
    """Block until the `perf` recorder is gone or `timeout_secs` elapses (bounded flush wait)."""
    deadline = time.monotonic() + timeout_secs
    while _perf_is_running() and time.monotonic() < deadline:
        time.sleep(poll_secs)


def start_system_wide_recording(*, perf_data: str) -> int:
    """Launch a detached system-wide `perf record` and return the launched PID (informational)."""
    dirname = os.path.dirname(perf_data)
    if dirname:
        os.makedirs(dirname, exist_ok=True)
    argv = build_perf_record_cmd(perf_data=perf_data)
    print("Starting:", " ".join(argv), file=sys.stderr)
    proc = subprocess.Popen(
        argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True
    )
    return proc.pid


def stop_system_wide_recording(
    *,
    timeout_secs: float = 60.0,
    poll_secs: float = 1.0,
) -> None:
    """SIGINT the `perf` recorder and wait, bounded, for it to flush perf.data and exit."""
    stop_argv = ["sudo", "pkill", "-INT", "-x", "perf"]
    print("Stopping:", " ".join(stop_argv), file=sys.stderr)
    subprocess.run(stop_argv, check=False)
    _wait_until_perf_stops(timeout_secs=timeout_secs, poll_secs=poll_secs)

    # If the recorder is still alive after the bounded wait (e.g. a missed signal), try once more.
    if _perf_is_running():
        print("Recorder still running after SIGINT; retrying pkill", file=sys.stderr)
        subprocess.run(stop_argv, check=False)
        _wait_until_perf_stops(timeout_secs=timeout_secs, poll_secs=poll_secs)


# --- Evergreen task-level orchestration ---
# `start-task` (before the benchmark suite) and `process-task` (after) are the two steps the
# Evergreen benchmarks_sep task runs.


def profiling_enabled(runtime_params_json: str) -> bool:
    """True when the Evergreen `runtime_params_json` requested on-CPU profiling (`enable_linux_perf`).

    Evergreen expands an undefined `${runtime_params_json}` to an empty string, so blank input reads
    as "not requested" rather than raising on `json.loads("")`.
    """
    params = json.loads(runtime_params_json.strip() or "{}")
    return params.get("enable_linux_perf") is True


def ensure_perf_installed() -> None:
    """Install `perf` and verify it runs."""
    for manager in ("yum", "dnf"):
        subprocess.run(["sudo", manager, "install", "-y", "perf", "perl"], check=False)
    subprocess.run(["perf", "--version"], check=True)


def gzip_file(src: str) -> str:
    """Gzip `src` to `src + ".gz"`, returning the new path and removing `src`."""
    dst = src + ".gz"
    with open(src, "rb") as f_in, gzip.open(dst, "wb") as f_out:
        shutil.copyfileobj(f_in, f_out)
    os.remove(src)
    return dst


def create_profiling_bundle(output_dir: str, bundle_name: str, members: list[str]) -> str:
    """Tar+gzip the `members` that exist under `output_dir` into `output_dir/bundle_name`."""
    bundle_path = os.path.join(output_dir, bundle_name)
    with tarfile.open(bundle_path, "w:gz") as tar:
        for member in members:
            member_path = os.path.join(output_dir, member)
            if os.path.isfile(member_path):
                tar.add(member_path, arcname=member)
    return bundle_path


def write_profiling_links(path: str, manifest: list[dict[str, str]]) -> None:
    """Write a links manifest as JSON."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w") as f:
        json.dump(manifest, f)


def _chown_to_current_user(path: str) -> None:
    """Reclaim ownership of the root-owned `perf record` output so this process can read it."""
    subprocess.run(["sudo", "chown", str(os.getuid()), path], check=True)


def start_task(
    *,
    runtime_params_json: str,
    output_dir: str = "profiles",
) -> int:
    """Evergreen pre-suite step: gate on the run params, install perf, start a detached recording."""
    if not profiling_enabled(runtime_params_json):
        print("on-CPU profiling not requested; skipping", file=sys.stderr)
        return 0
    ensure_perf_installed()
    os.makedirs(output_dir, exist_ok=True)
    start_system_wide_recording(perf_data=os.path.join(output_dir, PERF_DATA_FILENAME))
    return 0


def process_task(
    *,
    runtime_params_json: str,
    output_dir: str = "profiles",
    svg_link: str,
) -> int:
    """Evergreen post-suite step: stop the recording, render the flamegraph, bundle and link artifacts.

    On a non-profiling run this only leaves an empty links file behind so the downstream s3.put
    no-ops; when profiling was requested, an empty perf.data is a hard failure.
    """
    links_path = os.path.join(output_dir, PROFILING_LINKS_FILENAME)
    if not profiling_enabled(runtime_params_json):
        print("on-CPU profiling not requested; skipping", file=sys.stderr)
        write_profiling_links(links_path, [])
        return 0

    stop_system_wide_recording()

    perf_data = os.path.join(output_dir, PERF_DATA_FILENAME)
    _chown_to_current_user(perf_data)
    if not os.path.isfile(perf_data) or os.path.getsize(perf_data) == 0:
        print(
            "ERROR: perf.data missing or empty after stop; profiling was requested but produced no data",
            file=sys.stderr,
        )
        return 1

    os.makedirs(output_dir, exist_ok=True)
    render_flamegraph(
        perf_data=perf_data,
        flamegraph_dir=ensure_flamegraph_dir(),
        output_prefix=os.path.join(output_dir, TASK_NAME),
        title=TASK_NAME,
    )

    gzip_file(perf_data)
    create_profiling_bundle(
        output_dir,
        f"{TASK_NAME}_profiling.tar.gz",
        [
            f"{TASK_NAME}.folded",
            f"{TASK_NAME}.svg",
            PERF_DATA_FILENAME + ".gz",
        ],
    )
    write_profiling_links(
        links_path,
        [{"name": "Flamegraph (SVG, opens in browser)", "link": svg_link, "visibility": "public"}],
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--start-task",
        action="store_true",
        help="Evergreen pre-suite step: gate on run params, install perf, start recording.",
    )
    group.add_argument(
        "--process-task",
        action="store_true",
        help="Evergreen post-suite step: stop recording, render flamegraph, bundle and link "
        "artifacts.",
    )

    parser.add_argument(
        "--runtime-params-json",
        default="",
        help="The Evergreen ${runtime_params_json} expansion; profiling runs only when it sets "
        "enable_linux_perf.",
    )
    parser.add_argument(
        "--svg-link",
        default="",
        help="Public URL the flamegraph SVG will be uploaded to. Required with --process-task.",
    )

    args = parser.parse_args(argv)

    if args.start_task:
        return start_task(runtime_params_json=args.runtime_params_json)
    if args.process_task:
        if not args.svg_link:
            parser.error("--svg-link is required with --process-task")
        return process_task(
            runtime_params_json=args.runtime_params_json,
            svg_link=args.svg_link,
        )
    return 2


if __name__ == "__main__":
    sys.exit(main())
