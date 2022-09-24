#!/usr/bin/env python3
"""Runs clang-tidy in parallel and combines the the results for easier viewing."""

import argparse
import datetime
import json
import os
import subprocess
import sys
import threading
import queue
import time
from typing import Any, Dict, List, Optional
import multiprocessing
from pathlib import Path

import yaml

files_to_tidy = queue.SimpleQueue()
files_to_parse = queue.SimpleQueue()


def _clang_tidy_executor(clang_tidy_binary: str, clang_tidy_cfg: Dict[str, Any], output_dir: str):
    while True:
        clang_tidy_filename: Optional[Path] = files_to_tidy.get()
        if clang_tidy_filename is None:
            files_to_parse.put(None)
            files_to_tidy.put(None)
            break

        print(f"Running clang-tidy on {clang_tidy_filename}")
        clang_tidy_parent_dir = output_dir / clang_tidy_filename.parent
        os.makedirs(clang_tidy_parent_dir, exist_ok=True)

        output_filename_base = clang_tidy_parent_dir / clang_tidy_filename.name
        output_filename_fixes = output_filename_base.with_suffix(".yml")
        clang_tidy_command = [
            clang_tidy_binary, clang_tidy_filename, f"-export-fixes={output_filename_fixes}",
            f"-config={json.dumps(clang_tidy_cfg)}"
        ]
        proc = subprocess.run(clang_tidy_command, capture_output=True, check=False)
        if proc.returncode != 0:
            output_filename_out = output_filename_base.with_suffix(".fail")
            files_to_parse.put(output_filename_fixes)
            print(
                f"Running clang-tidy on {clang_tidy_filename} had errors see {output_filename_out}")
        else:
            output_filename_out = output_filename_base.with_suffix(".pass")
            print(f"Running clang-tidy on {clang_tidy_filename} had no errors")

        with open(output_filename_out, 'wb') as output:
            output.write(proc.stderr)
            output.write(proc.stdout)


def _combine_errors(clang_tidy_executors: int, fixes_filename: str) -> int:
    failed_files = 0
    all_fixes = {}
    while clang_tidy_executors > 0:
        item = files_to_parse.get()

        # Once all running threads say they are done we want to exit
        if item is None:
            clang_tidy_executors -= 1
            continue

        failed_files += 1

        # Read the yaml fixes for the file to combine them with the other suggested fixes
        with open(item) as input_yml:
            fixes = yaml.safe_load(input_yml)
        for fix in fixes['Diagnostics']:
            fix_data = all_fixes.setdefault(fix["DiagnosticName"], {}).setdefault(
                fix["FilePath"], {}).setdefault(
                    fix["FileOffset"], {
                        "replacements": fix["Replacements"], "message": fix["Message"], "count": 0,
                        "source_files": []
                    })
            fix_data["count"] += 1
            fix_data["source_files"].append(fixes['MainSourceFile'])
    with open(fixes_filename, "w") as files_file:
        json.dump(all_fixes, files_file, indent=4, sort_keys=True)

    return failed_files


def _report_status(total_jobs: int, clang_tidy_executor_threads: List[threading.Thread]):
    start_time = time.time()
    running_jobs = 1
    while running_jobs > 0:
        time.sleep(5)
        pretty_time_duration = str(datetime.timedelta(seconds=time.time() - start_time))
        running_jobs = sum(
            [1 for t in clang_tidy_executor_threads if t.is_alive()])  # Count threads running a job
        # files_to_tidy contains a None which can be ignored
        print(
            f"There are {running_jobs} active jobs. The number of jobs queued is {files_to_tidy.qsize()-1}/{total_jobs}. Duration {pretty_time_duration}."
        )


def main():
    """Execute Main entry point."""

    parser = argparse.ArgumentParser(description='Run multithreaded clang-tidy')

    parser.add_argument('-t', "--threads", type=int, default=multiprocessing.cpu_count(),
                        help="Run with a specific number of threads")
    parser.add_argument("-d", "--output-dir", type=str, default="clang_tidy_fixes",
                        help="Directory to write all clang-tidy output to")
    parser.add_argument("-o", "--fixes-file", type=str, default="clang_tidy_fixes.json",
                        help="Report json file to write combined fixes to")
    parser.add_argument("-c", "--compile-commands", type=str, default="compile_commands.json",
                        help="compile_commands.json file to use to find the files to tidy")
    # TODO: Is there someway to get this without hardcoding this much
    parser.add_argument("-y", "--clang-tidy-toolchain", type=str, default="v3")
    parser.add_argument("-f", "--clang-tidy-cfg", type=str, default=".clang-tidy")
    args = parser.parse_args()

    clang_tidy_binary = f'/opt/mongodbtoolchain/{args.clang_tidy_toolchain}/bin/clang-tidy'

    with open(args.compile_commands) as compile_commands:
        compile_commands = json.load(compile_commands)

    with open(args.clang_tidy_cfg) as clang_tidy_cfg:
        clang_tidy_cfg = yaml.safe_load(clang_tidy_cfg)

    for file_doc in compile_commands:
        # A few special cases of files to ignore
        if not "src/mongo" in file_doc["file"]:
            continue
        # TODO SERVER-49884 Remove this when we no longer check in generated Bison.
        if "parser_gen.cpp" in file_doc["file"]:
            continue
        files_to_tidy.put(Path(file_doc["file"]))

    total_jobs = files_to_tidy.qsize()
    files_to_tidy.put(None)
    workers = args.threads

    clang_tidy_executor_threads: List[threading.Thread] = []
    for _ in range(workers):
        clang_tidy_executor_threads.append(
            threading.Thread(target=_clang_tidy_executor, args=(clang_tidy_binary, clang_tidy_cfg,
                                                                args.output_dir)))
        clang_tidy_executor_threads[-1].start()

    report_status_thread = threading.Thread(target=_report_status,
                                            args=(total_jobs, clang_tidy_executor_threads))
    report_status_thread.start()

    failed_files = _combine_errors(workers, Path(args.output_dir, args.fixes_file))

    # Join all threads
    report_status_thread.join()
    for thread in clang_tidy_executor_threads:
        thread.join()

    # Zip up all the files for upload
    subprocess.run(["tar", "-czvf", args.output_dir + ".tgz", args.output_dir], check=False)

    return failed_files


if __name__ == "__main__":
    sys.exit(main())
