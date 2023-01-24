#!/usr/bin/env python3
"""Runs clang-tidy in parallel and combines the the results for easier viewing."""

import argparse
import datetime
import json
import os
import subprocess
import sys
import locale
import hashlib
import time
from typing import Any, Dict, List, Optional, Tuple
import multiprocessing
from pathlib import Path
from concurrent import futures
from simple_report import Result, Report, put_report, try_combine_reports, make_report
import yaml


def _clang_tidy_executor(clang_tidy_filename: str, clang_tidy_binary: str,
                         clang_tidy_cfg: Dict[str, Any], output_dir: str, show_stdout: bool,
                         mongo_check_module: str = '') -> Tuple[str, Optional[str]]:

    clang_tidy_parent_dir = output_dir / clang_tidy_filename.parent
    os.makedirs(clang_tidy_parent_dir, exist_ok=True)

    output_filename_base = clang_tidy_parent_dir / clang_tidy_filename.name
    output_filename_fixes = output_filename_base.with_suffix(".yml")

    if mongo_check_module:
        load_module_option = ['-load', mongo_check_module]
    else:
        load_module_option = []

    clang_tidy_command = [
        clang_tidy_binary, *load_module_option, clang_tidy_filename,
        f"-export-fixes={output_filename_fixes}", f"-config={json.dumps(clang_tidy_cfg)}"
    ]
    proc = subprocess.run(clang_tidy_command, capture_output=True, check=False)
    files_to_parse = None
    if proc.returncode != 0:
        output_filename_out = output_filename_base.with_suffix(".fail")
        files_to_parse = output_filename_fixes
        if not show_stdout:
            print(
                f"Running clang-tidy on {clang_tidy_filename} had errors see {output_filename_out}")
        else:
            print(f"Running clang-tidy on {clang_tidy_filename}")
            print(f"{proc.stderr.decode(locale.getpreferredencoding())}")
            print(f"{proc.stdout.decode(locale.getpreferredencoding())}")
    else:
        output_filename_out = output_filename_base.with_suffix(".pass")
        if not show_stdout:
            print(f"Running clang-tidy on {clang_tidy_filename} had no errors")

    with open(output_filename_out, 'wb') as output:
        output.write(proc.stderr)
        output.write(proc.stdout)
    return proc.stdout.decode(locale.getpreferredencoding()), files_to_parse


def _combine_errors(fixes_filename: str, files_to_parse: List[str]) -> int:
    failed_files = 0
    all_fixes = {}

    #loop files_to_parse and count the number of failed_files
    for item in files_to_parse:
        if item is None:
            continue
        failed_files += 1

        # Read the yaml fixes for the file to combine them with the other suggested fixes
        with open(item) as input_yml:
            fixes = yaml.safe_load(input_yml)
        for fix in fixes['Diagnostics']:
            fix_msg = fix["DiagnosticMessage"]
            fix_data = all_fixes.setdefault(fix["DiagnosticName"], {}).setdefault(
                fix_msg.get("FilePath", "FilePath Not Found"), {}).setdefault(
                    str(fix_msg.get("FileOffset", "FileOffset Not Found")), {
                        "replacements": fix_msg.get(
                            "Replacements", "Replacements not found"), "message": fix_msg.get(
                                "Message", "Message not found"), "count": 0, "source_files": []
                    })
            fix_data["count"] += 1
            fix_data["source_files"].append(fixes['MainSourceFile'])
            if fix_msg.get("FilePath") and os.path.exists(fix_msg.get("FilePath")):
                all_fixes[fix["DiagnosticName"]][fix_msg.get("FilePath")]['md5'] = hashlib.md5(
                    open(fix_msg.get("FilePath"), 'rb').read()).hexdigest()

    with open(fixes_filename, "w") as files_file:
        json.dump(all_fixes, files_file, indent=4, sort_keys=True)

    return failed_files


def __dedup_errors(clang_tidy_errors_threads: List[str]) -> str:
    #use dict as an 'ordered set'(in python 3.6+), set value to dummy value(true here)
    error_to_dummy_value = dict()
    for errs in clang_tidy_errors_threads:
        if errs:
            for val in errs.splitlines():
                error_to_dummy_value[val] = True
    return os.linesep.join(error_to_dummy_value.keys())


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
    parser.add_argument("-q", "--show-stdout", type=bool, default=True,
                        help="Log errors to console")
    parser.add_argument("-l", "--log-file", type=str, default="clang_tidy",
                        help="clang tidy log from evergreen")
    parser.add_argument("-m", "--check-module", type=str,
                        default="build/install/lib/libmongo_tidy_checks.so",
                        help="Path to load the custom mongo checks module.")
    # TODO: Is there someway to get this without hardcoding this much
    parser.add_argument("-y", "--clang-tidy-toolchain", type=str, default="v4")
    parser.add_argument("-f", "--clang-tidy-cfg", type=str, default=".clang-tidy")
    args = parser.parse_args()

    clang_tidy_binary = f'/opt/mongodbtoolchain/{args.clang_tidy_toolchain}/bin/clang-tidy'

    if os.path.exists(args.check_module):
        mongo_tidy_check_module = args.check_module
    else:
        mongo_tidy_check_module = ''

    if os.path.exists(args.compile_commands):
        with open(args.compile_commands) as compile_commands:
            compile_commands = json.load(compile_commands)
    else:
        if args.compile_commands == parser.get_default('compile_commands'):
            print(
                f"Could not find compile commands: '{args.compile_commands}', to generate it, use the build command:\n\n"
                + "python3 buildscripts/scons.py --build-profile=compiledb compiledb\n")
        else:
            print(f"Could not find compile commands: {args.compile_commands}")
        sys.exit(1)

    if os.path.exists(args.clang_tidy_cfg):
        with open(args.clang_tidy_cfg) as clang_tidy_cfg:
            clang_tidy_cfg = yaml.safe_load(clang_tidy_cfg)
    else:
        if args.clang_tidy_cfg == parser.get_default('clang_tidy_cfg'):
            print(
                f"Could not find config file: '{args.clang_tidy_cfg}', to generate it, use the build command:\n\n"
                + "python3 buildscripts/scons.py --build-profile=compiledb compiledb\n")
        else:
            print(f"Could not find config file: {args.clang_tidy_cfg}")
        sys.exit(1)

    files_to_tidy = list()
    files_to_parse = list()
    for file_doc in compile_commands:
        # A few special cases of files to ignore
        if not "src/mongo" in file_doc["file"]:
            continue
        # TODO SERVER-49884 Remove this when we no longer check in generated Bison.
        if "parser_gen.cpp" in file_doc["file"]:
            continue
        files_to_tidy.append(Path(file_doc["file"]))

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
                executor.submit(_clang_tidy_executor, clang_tidy_filename, clang_tidy_binary,
                                clang_tidy_cfg, args.output_dir, args.show_stdout,
                                mongo_tidy_check_module))

        for future in futures.as_completed(clang_tidy_executor_futures):
            clang_tidy_errors_futures.append(future.result()[0])
            files_to_parse.append(future.result()[1])
            tasks_completed += 1
            pretty_time_duration = str(datetime.timedelta(seconds=time.time() - start_time))
            print(
                f" The number of jobs completed is {tasks_completed}/{total_jobs}. Duration {pretty_time_duration}"
            )

    failed_files = _combine_errors(Path(args.output_dir, args.fixes_file), files_to_parse)

    # Zip up all the files for upload
    subprocess.run(["tar", "-czvf", args.output_dir + ".tgz", args.output_dir], check=False)

    # create report and dump to report.json
    error_file_contents = __dedup_errors(clang_tidy_errors_futures)
    report = make_report(args.log_file, error_file_contents, 1 if failed_files > 0 else 0)
    try_combine_reports(report)
    put_report(report)

    return failed_files


if __name__ == "__main__":
    sys.exit(main())
