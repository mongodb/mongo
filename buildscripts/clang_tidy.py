#!/usr/bin/env python3
"""Runs clang-tidy in parallel and combines the the results for easier viewing."""

import argparse
import datetime
import hashlib
import json
import locale
import math
import multiprocessing
import os
import re
import subprocess
import sys
import time
from concurrent import futures
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

import yaml
from clang_tidy_vscode import CHECKS_SO
from simple_report import make_report, put_report, try_combine_reports


def _clang_tidy_executor(
    clang_tidy_filename: Path,
    clang_tidy_binary: str,
    clang_tidy_cfg: Dict[str, Any],
    output_dir: str,
    show_stdout: bool,
    mongo_check_module: str = "",
    compile_commands: str = "compile_commands.json",
) -> Tuple[str, Optional[str]]:
    clang_tidy_parent_dir = output_dir / clang_tidy_filename.parent
    os.makedirs(clang_tidy_parent_dir, exist_ok=True)

    output_filename_base = clang_tidy_parent_dir / clang_tidy_filename.name
    output_filename_fixes = output_filename_base.with_suffix(".yml")

    if mongo_check_module:
        load_module_option = ["-load", mongo_check_module]
    else:
        load_module_option = []

    clang_tidy_command = [
        clang_tidy_binary,
        *load_module_option,
        "-p",
        os.path.dirname(compile_commands),
        clang_tidy_filename,
        f"-export-fixes={output_filename_fixes}",
        f"-config={json.dumps(clang_tidy_cfg)}",
    ]
    proc = subprocess.run(clang_tidy_command, capture_output=True, check=False)
    files_to_parse = None
    if proc.returncode != 0:
        output_filename_out = output_filename_base.with_suffix(".fail")
        files_to_parse = output_filename_fixes
        if not show_stdout:
            print(
                f"Running clang-tidy on {clang_tidy_filename} had errors see {output_filename_out}"
            )
        else:
            print(f"Running clang-tidy on {clang_tidy_filename}")
            print(f"{proc.stderr.decode(locale.getpreferredencoding())}")
            print(f"{proc.stdout.decode(locale.getpreferredencoding())}")
    else:
        output_filename_out = output_filename_base.with_suffix(".pass")
        if not show_stdout:
            print(f"Running clang-tidy on {clang_tidy_filename} had no errors")

    with open(output_filename_out, "wb") as output:
        output.write(proc.stderr)
        output.write(proc.stdout)
    return proc.stdout.decode(locale.getpreferredencoding()), files_to_parse


def _combine_errors(fixes_filename: str, files_to_parse: List[str]) -> int:
    failed_files = 0
    all_fixes = {}

    # loop files_to_parse and count the number of failed_files
    for item in files_to_parse:
        if item is None:
            continue
        failed_files += 1

        # Read the yaml fixes for the file to combine them with the other suggested fixes
        with open(item) as input_yml:
            fixes = yaml.safe_load(input_yml)
        for fix in fixes["Diagnostics"]:
            fix_msg = None
            if "Notes" in fix:
                fix_msg = fix["Notes"][0]
                if len(fix["Notes"]) > 1:
                    print(f'Warning: this script may be missing values in [{fix["Notes"]}]')
            else:
                fix_msg = fix["DiagnosticMessage"]
            fix_data = (
                all_fixes.setdefault(fix["DiagnosticName"], {})
                .setdefault(fix_msg.get("FilePath", "FilePath Not Found"), {})
                .setdefault(
                    str(fix_msg.get("FileOffset", "FileOffset Not Found")),
                    {
                        "replacements": fix_msg.get("Replacements", "Replacements not found"),
                        "message": fix_msg.get("Message", "Message not found"),
                        "count": 0,
                        "source_files": [],
                    },
                )
            )
            fix_data["count"] += 1
            fix_data["source_files"].append(fixes["MainSourceFile"])
            if fix_msg.get("FilePath") and os.path.exists(fix_msg.get("FilePath")):
                all_fixes[fix["DiagnosticName"]][fix_msg.get("FilePath")]["md5"] = hashlib.md5(
                    open(fix_msg.get("FilePath"), "rb").read()
                ).hexdigest()

    with open(fixes_filename, "w") as files_file:
        json.dump(all_fixes, files_file, indent=4, sort_keys=True)

    return failed_files


def __dedup_errors(clang_tidy_errors_threads: List[str]) -> str:
    unique_single_errors = set()
    for errs in clang_tidy_errors_threads:
        if errs:
            lines = errs.splitlines()
            single_error_start_line = 0
            for i, line in enumerate(lines):
                if line:
                    # the first line of one single error message like:
                    # ......./d_concurrency.h:175:13: error: .........
                    # trying to match  :lineNumber:colomnNumber:
                    matched_regex = re.match("(.+:[0-9]+:[0-9]+:)", line)

                    # Collect a full single error message
                    # when we find another match or reach the last line of the text
                    if matched_regex and i != single_error_start_line:
                        unique_single_errors.add(tuple(lines[single_error_start_line:i]))
                        single_error_start_line = i
                    elif i == len(lines) - 1:
                        unique_single_errors.add(tuple(lines[single_error_start_line : i + 1]))

    unique_single_error_flatten = [item for sublist in unique_single_errors for item in sublist]
    return os.linesep.join(unique_single_error_flatten)


def _run_tidy(args, parser_defaults):
    clang_tidy_binary = f"/opt/mongodbtoolchain/{args.clang_tidy_toolchain}/bin/clang-tidy"

    if os.path.exists(args.check_module):
        mongo_tidy_check_module = args.check_module
    else:
        mongo_tidy_check_module = ""

    if os.path.exists(args.compile_commands):
        with open(args.compile_commands) as compile_commands:
            compile_commands = sorted(json.load(compile_commands), key=lambda x: x["file"])
    else:
        if args.compile_commands == parser_defaults.compile_commands:
            print(
                f"Could not find compile commands: '{args.compile_commands}', to generate it, use the build command:\n\n"
                + "python3 buildscripts/scons.py --build-profile=compiledb compiledb\n"
            )
        else:
            print(f"Could not find compile commands: {args.compile_commands}")
        sys.exit(1)

    if os.path.exists(args.clang_tidy_cfg):
        with open(args.clang_tidy_cfg) as clang_tidy_cfg:
            clang_tidy_cfg = yaml.safe_load(clang_tidy_cfg)
    else:
        if args.clang_tidy_cfg == parser_defaults.clang_tidy_cfg:
            print(
                f"Could not find config file: '{args.clang_tidy_cfg}', to generate it, use the build command:\n\n"
                + "python3 buildscripts/scons.py --build-profile=compiledb compiledb\n"
            )
        else:
            print(f"Could not find config file: {args.clang_tidy_cfg}")
        sys.exit(1)

    if args.split_jobs < 0:
        print(f"--split-jobs: '{args.split_jobs}' must positive integer.")
        sys.exit(1)

    if args.split_jobs != 0:
        if args.split < 1 or args.split > args.split_jobs:
            print(
                f"--split: '{args.split}' must be a value between 1 and --split-jobs: '{args.split_jobs}'"
            )
            sys.exit(1)

    files_to_tidy: List[Path] = list()
    files_to_parse = list()
    filtered_compile_commands = []
    for file_doc in compile_commands:
        # A few special cases of files to ignore
        if not file_doc["file"].startswith("src/mongo/"):
            continue

        # Don't run clang_tidy on the streams/third_party code.
        if file_doc["file"].startswith("src/mongo/db/modules/enterprise/src/streams/third_party"):
            continue

        # TODO SERVER-49884 Remove this when we no longer check in generated Bison.
        if file_doc["file"].endswith("/parser_gen.cpp"):
            continue
        filtered_compile_commands.append(file_doc)

    if args.split_jobs != 0:
        original_compile_commands = filtered_compile_commands.copy()
        total = len(filtered_compile_commands)
        extra = total % args.split_jobs
        chunk_size = int(total / args.split_jobs) + math.ceil(extra / args.split_jobs)
        chunks = [
            filtered_compile_commands[i : i + chunk_size]
            for i in range(0, len(filtered_compile_commands), chunk_size)
        ]
        # verify we aren't silently forgetting anything.
        for chunk in chunks:
            for file_doc in chunk:
                original_compile_commands.remove(file_doc)
        if len(original_compile_commands) != 0:
            raise Exception(
                "Total compile_commands different from sum of splits! This means something could silently be ignored!"
            )
        filtered_compile_commands = chunks[args.split - 1]

    files_to_tidy = [Path(file_doc["file"]) for file_doc in filtered_compile_commands]

    total_jobs = len(files_to_tidy)
    workers = args.threads

    clang_tidy_errors_futures: List[str] = []
    clang_tidy_executor_futures: List[futures.ThreadPoolExecutor.submit] = []

    # total completed tasks
    tasks_completed = 0

    with futures.ThreadPoolExecutor(max_workers=workers) as executor:
        start_time = time.time()

        # submit all futures
        for clang_tidy_filename in files_to_tidy:
            clang_tidy_executor_futures.append(
                executor.submit(
                    _clang_tidy_executor,
                    clang_tidy_filename,
                    clang_tidy_binary,
                    clang_tidy_cfg,
                    args.output_dir,
                    args.show_stdout,
                    mongo_tidy_check_module,
                    compile_commands=args.compile_commands,
                )
            )

        for future in futures.as_completed(clang_tidy_executor_futures):
            clang_tidy_errors_futures.append(future.result()[0])
            files_to_parse.append(future.result()[1])
            tasks_completed += 1
            pretty_time_duration = str(datetime.timedelta(seconds=time.time() - start_time))
            print(
                f" The number of jobs completed is {tasks_completed}/{total_jobs}. Duration {pretty_time_duration}"
            )

    return clang_tidy_errors_futures, files_to_parse


def main():
    """Execute Main entry point."""

    parser = argparse.ArgumentParser(description="Run multithreaded clang-tidy")

    parser.add_argument(
        "-t",
        "--threads",
        type=int,
        default=multiprocessing.cpu_count(),
        help="Run with a specific number of threads",
    )
    parser.add_argument(
        "-d",
        "--output-dir",
        type=str,
        default="clang_tidy_fixes",
        help="Directory to write all clang-tidy output to",
    )
    parser.add_argument(
        "-o",
        "--fixes-file",
        type=str,
        default="clang_tidy_fixes.json",
        help="Report json file to write combined fixes to",
    )
    parser.add_argument(
        "-c",
        "--compile-commands",
        type=str,
        default="compile_commands.json",
        help="compile_commands.json file to use to find the files to tidy",
    )
    parser.add_argument(
        "--split-jobs",
        type=int,
        default=0,
        help="The total number of splits if splitting the jobs across multiple tasks. 0 means don't split.",
    )
    parser.add_argument(
        "--split",
        type=int,
        default=1,
        help="The interval to run out of the total number of --split-jobs. Must be a value between 1 and --split-jobs value.",
    )
    parser.add_argument(
        "-q", "--show-stdout", type=bool, default=True, help="Log errors to console"
    )
    parser.add_argument(
        "-l", "--log-file", type=str, default="clang_tidy", help="clang tidy log from evergreen"
    )
    parser.add_argument(
        "--only-process-fixes",
        action="store_true",
        help="Skip tidy and process the fixes directory to generate a fixes file. Use in conjunction with -d.",
    )
    parser.add_argument(
        "--disable-reporting",
        action="store_true",
        default=False,
        help="Disable generating the report file for evergreen perf.send",
    )
    parser.add_argument(
        "-m",
        "--check-module",
        type=str,
        default=CHECKS_SO,
        help="Path to load the custom mongo checks module.",
    )
    # TODO: Is there someway to get this without hardcoding this much
    parser.add_argument("-y", "--clang-tidy-toolchain", type=str, default="v4")
    parser.add_argument("-f", "--clang-tidy-cfg", type=str, default=".clang-tidy")
    args = parser.parse_args()

    if args.only_process_fixes:
        if not os.path.isdir(args.output_dir):
            print(f"Error: {args.output_dir} is not a valid directory.")
            sys.exit(3)
        find_cmd = ["find", args.output_dir, "-type", "f", "-name", "*.yml"]
        find_output = subprocess.Popen(find_cmd, stdout=subprocess.PIPE)
        files_to_parse = []
        for line in iter(find_output.stdout.readline, ""):
            if not line:
                break
            files_to_parse.append(str(line.rstrip().decode("utf-8")))
    else:
        parser_defaults = parser.parse_args([])
        clang_tidy_errors_futures, files_to_parse = _run_tidy(args, parser_defaults)

    failed_files = _combine_errors(Path(args.output_dir, args.fixes_file), files_to_parse)

    if not args.only_process_fixes:
        # Zip up all the files for upload
        subprocess.run(["tar", "-czvf", args.output_dir + ".tgz", args.output_dir], check=False)

        # Create report and dump to report.json
        if not args.disable_reporting:
            error_file_contents = __dedup_errors(clang_tidy_errors_futures)
            report = make_report(args.log_file, error_file_contents, 1 if failed_files > 0 else 0)
            try_combine_reports(report)
            put_report(report)

    return failed_files


if __name__ == "__main__":
    sys.exit(main())
