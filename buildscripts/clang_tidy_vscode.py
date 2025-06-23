#!/usr/bin/env python3
"""
Wraps clang tidy to include our custom checks.

This script acts as a wrapper for the `clang-tidy` tool, allowing it to include custom checks
defined in a shared object file (`libmongo_tidy_checks.so`). Additionally, it filters the files
to be checked, ensuring that only files within the `src/mongo` directory are processed, excluding
those within `src/mongo/db/modules/enterprise/src/streams/third_party`.

Input:
- The script expects command-line arguments that are passed to `clang-tidy`.
- These arguments can include file paths, options, and other parameters supported by `clang-tidy`.

Output:
- The script runs `clang-tidy` on the specified files and outputs the results.
- If no valid `.cpp` files are found, or if all `.cpp` files are located in the excluded directories,
  the script skips running `clang-tidy`.
- Standard output and error from the `clang-tidy` process are captured and printed.

Expected Format:
- command line example: buildscripts/clang_tidy_vscode.py /path/to/file/filename --export-fixes=-
- buildscripts/clang_tidy_vscode.py /path/to/file/filename --export-fixes=-
"""

# TODO: if https://github.com/notskm/vscode-clang-tidy/pull/77#issuecomment-1422910143 is resolved then this script can be removed

import json
import multiprocessing
import os
import pathlib
import subprocess
import sys
import time

from mongo_toolchain import get_mongo_toolchain

CLTCONFIG = """
# This file is intended to document the configuration options available
[preprocessor]
# Compiler command used when running preprocessor
command=%s
# Ignore errors from preprocessor and use whatever output it generated as cache key
# May cause weird issues when no output is generated
ignore_errors=false

# "" => NOLINT-comments don't work properly
# "-C" => NOLINT-comments work for regular code, but not in preprocessor macro expansion
# "-CC" => NOLINT-comments should work everywhere, but valid code may fail preprocessor stage. Combine with ignore_errors if you are paranoid about issues with NOLINT-comments
preserve_comments=-C

# Increase cache hit rate by ignoring some types of string contents
strip_string_versions=true
strip_string_hex_hashes=true

[behavior]
# Cache results even when clang-tidy fails
cache_failure=true
# Print cltcache errors and info in stderr and stdout
verbose=false
"""


def get_half_cpu_mask():
    num_cores = multiprocessing.cpu_count()
    return max(1, num_cores // 2)


def count_running_clang_tidy(cmd_path):
    try:
        output = subprocess.check_output(["ps", "-axww", "-o", "pid=,command="], text=True)
        return sum(1 for line in output.splitlines() if cmd_path in line)
    except Exception as e:
        print(f"WARNING: failed to check running clang-tidy processes: {e}")
        return 0


def wait_for_available_slot(cmd_path, max_jobs, check_interval):
    while True:
        if count_running_clang_tidy(cmd_path) < max_jobs:
            break
        time.sleep(check_interval)


def main():
    cltcache_path = pathlib.Path(__file__).parent / "cltcache" / "cltcache.py.txt"

    toolchain = get_mongo_toolchain(version="v5", from_bazel=True)
    clang_tidy_path = toolchain.get_tool_path("clang-tidy")
    max_jobs = get_half_cpu_mask()
    wait_for_available_slot(clang_tidy_path, max_jobs, check_interval=0.2)

    clang_tidy_cmd = [clang_tidy_path]

    checks_so = None
    if os.path.exists(".mongo_checks_module_path"):
        with open(".mongo_checks_module_path") as f:
            checks_so = f.read().strip()

    if checks_so and os.path.isfile(checks_so):
        clang_tidy_cmd += [f"-load={checks_so}"]
    else:
        print("ERROR: failed to find mongo tidy checks, run `bazel build compiledb'")
        sys.exit(1)

    files_to_check = []
    other_args = []
    for arg in sys.argv[1:]:
        if os.path.isfile(arg):
            rel = os.path.relpath(arg, os.path.dirname(os.path.dirname(__file__)))
            if (
                (arg.endswith(".cpp") or arg.endswith(".h"))
                and rel.startswith("src/mongo")
                and not rel.startswith("src/mongo/db/modules/enterprise/src/streams/third_party")
            ):
                files_to_check.append(rel)
        else:
            other_args.append(arg)

    if not files_to_check:
        return 0

    if len(files_to_check) > 1:
        print(
            f"ERROR: more than one file passed: {files_to_check}, only running {files_to_check[0]}"
        )

    if not os.path.exists("compile_commands.json"):
        print("ERROR: failed to find compile_commands.json, run 'bazel build compiledb'")
        sys.exit(1)

    with open("compile_commands.json") as f:
        compdb = json.load(f)

    compile_args = []
    executable = None
    for entry in compdb:
        if entry["file"] == files_to_check[0]:
            compile_args = entry["arguments"][1:]
            executable = entry["arguments"][0]
            try:
                index = compile_args.index("-MD")
                compile_args = compile_args[:index] + compile_args[index + 3 :]
            except ValueError:
                pass
            break

    # found a cpp file entry with exact compile args, cache it
    if compile_args:
        cfg_dir = pathlib.Path().home() / ".cltcache"
        cfg_dir.mkdir(parents=True, exist_ok=True)

        conf_file = cfg_dir / "cltcache.cfg"
        new_content = CLTCONFIG % executable

        if not conf_file.exists() or conf_file.read_text() != new_content:
            conf_file.write_text(new_content)

        full_cmd = (
            [sys.executable, cltcache_path]
            + clang_tidy_cmd
            + files_to_check
            + other_args
            + ["--"]
            + compile_args
        )

        proc = subprocess.run(full_cmd, capture_output=True)

    # probably a header, skip caching and let clang-tidy do its thing:
    else:
        proc = subprocess.run(clang_tidy_cmd + files_to_check + other_args, capture_output=True)

    sys.stdout.buffer.write(proc.stdout)
    sys.stderr.buffer.write(proc.stderr)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
