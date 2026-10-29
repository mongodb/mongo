#!/usr/bin/env python3
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

# Convert raw coverage data (.gcda/.gcno) into gcovr JSON tracefiles using multiple
# gcovr processes. gcovr's own -j option only creates threads, which are GIL-bound
# while parsing gcov output, so process-level parallelism is required to use more
# than one core. The tracefiles this script produces are meant to be combined with
# 'gcovr --add-tracefile' afterwards.

import argparse
import logging
import os
import re
import subprocess
import sys
from datetime import datetime

# Flags shared by every gcovr invocation that parses raw gcov data.
# - WiredTiger's internal functions are prefixed with '__' which gcovr would
#   otherwise treat as compiler-generated symbols and silently exclude.
# - Negative and suspicious counter values are expected when profiling
#   multi-threaded programs (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68080);
#   they are recorded as zero hits.
GCOV_PARSE_FLAGS = [
    "--include-internal-functions",
    "--gcov-ignore-parse-errors=negative_hits.warn_once_per_file",
    "--gcov-ignore-parse-errors=suspicious_hits.warn_once_per_file",
]


def find_coverage_data(search_dir):
    # Collect .gcda files, plus .gcno files of objects that never ran so they
    # still show up as uncovered in the report.
    data_files = list()
    for root, _, files in os.walk(search_dir):
        names = set(files)
        for name in names:
            if name.endswith(".gcda"):
                data_files.append(os.path.join(root, name))
            elif name.endswith(".gcno") and name[:-len(".gcno")] + ".gcda" not in names:
                data_files.append(os.path.join(root, name))
    return data_files


def split_into_groups(data_files, num_groups):
    # Greedily assign the largest files first to the least-loaded group so the
    # parallel gcovr processes finish at roughly the same time.
    groups = [list() for _ in range(num_groups)]
    loads = [0] * num_groups
    for path in sorted(data_files, key=os.path.getsize, reverse=True):
        index = loads.index(min(loads))
        groups[index].append(path)
        loads[index] += os.path.getsize(path)
    return [group for group in groups if group]


def gcov_major_version():
    # Version of the gcov resolved from PATH, which is the one gcovr will run.
    try:
        output = subprocess.run(["gcov", "--version"], capture_output=True,
                                text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    match = re.search(r"\s(\d+)\.\d+\.\d+", output)
    return int(match.group(1)) if match else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-j', '--jobs', default=os.cpu_count() or 1, type=int,
                        help='How many gcovr processes to run in parallel')
    parser.add_argument('-f', '--filter', default='src',
                        help='gcovr filter for source files to report on')
    parser.add_argument('-o', '--output_dir', required=True,
                        help='Directory to write the JSON tracefiles to')
    parser.add_argument('-s', '--search_dir', default='.',
                        help='Directory to search for coverage data files')
    parser.add_argument('-v', '--verbose', action="store_true", help='Be verbose')
    args = parser.parse_args()

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO)

    if args.jobs < 1:
        sys.exit("Number of jobs must be >= 1")

    # Only gcov 12 and newer name their intermediate files uniquely per data
    # file; older versions hash the source path, so concurrent gcovr
    # processes handling the same sources clobber each other's files, and gcovr's
    # directory lock only guards against its own threads.
    if args.jobs > 1:
        gcov_version = gcov_major_version()
        if gcov_version is None or gcov_version < 12:
            logging.warning(
                "gcov major version is %s; parallel gcovr processes require >= 12, "
                "falling back to a single process", gcov_version)
            args.jobs = 1

    data_files = find_coverage_data(args.search_dir)
    if not data_files:
        sys.exit(f"No coverage data files found under {args.search_dir}")

    groups = split_into_groups(data_files, args.jobs)
    logging.info("Converting %d coverage data files using %d gcovr processes",
                 len(data_files), len(groups))

    # Remove tracefiles left by a previous run: the merge step globs every JSON
    # file in this directory, so stale ones would be double-counted.
    os.makedirs(args.output_dir, exist_ok=True)
    for name in os.listdir(args.output_dir):
        if name.endswith(".json"):
            os.remove(os.path.join(args.output_dir, name))

    start_time = datetime.now()
    processes = list()
    for index, group in enumerate(groups):
        tracefile = os.path.join(args.output_dir, f"tracefile_{index:03d}.json")
        command = ["gcovr", *GCOV_PARSE_FLAGS, "-f", args.filter,
                   "--json", tracefile, *group]
        logging.debug("Starting gcovr for %d data files -> %s", len(group), tracefile)
        processes.append(subprocess.Popen(command))

    failed = 0
    for process in processes:
        if process.wait() != 0:
            logging.error("gcovr failed with exit code %d: %s",
                          process.returncode, ' '.join(process.args[:20]))
            failed += 1

    elapsed = (datetime.now() - start_time).total_seconds()
    logging.info("Finished converting coverage data in %.1f seconds", elapsed)

    if failed:
        sys.exit(f"{failed} of {len(processes)} gcovr processes failed")


if __name__ == '__main__':
    main()
