#!/usr/bin/python
# -*- coding: utf-8 -*-

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

import argparse
import os.path
import platform
import psutil
import subprocess
import sys
import json
from perf_stat import PerfStat
from perf_stat_collection import PerfStatCollection
from typing import Dict, List, Tuple
from perf_config import PerfConfig, TestType


def create_test_home_path(home: str, test_run: int, index: int):
    home_path = "{}_{}_{}".format(home, index, test_run)
    return home_path


def construct_command_line(exec_path: str, test_arg: List[str], home_arg: List[str], arguments: List[str]):
    command_line = []
    command_line.append(exec_path)
    if test_arg is not None:
        command_line.extend(test_arg)
    if arguments is not None:
        command_line.extend(arguments)
    if home_arg is not None:
        command_line.extend(home_arg)
    return command_line


def to_value_list(reported_stats: List[PerfStat], brief: bool):
    stats_list = []
    for stat in reported_stats:
        stat_list = stat.get_value_list(brief=brief)
        stats_list.extend(stat_list)
    return stats_list


def brief_perf_stats(config: PerfConfig, reported_stats: List[PerfStat]):
    as_list = [{
        "info": {
            "test_name": os.path.basename(config.test)
        },
        "metrics": to_value_list(reported_stats, brief=True)
    }]
    return as_list


def detailed_perf_stats(config: PerfConfig, reported_stats: List[PerfStat]):
    total_memory_gb = psutil.virtual_memory().total / (1024 * 1024 * 1024)
    as_dict = {
                'Test Name': os.path.basename(config.test),
                'config': config.to_value_dict(),
                'metrics': to_value_list(reported_stats, brief=False),
                'system': {
                   'cpu_physical_cores': psutil.cpu_count(logical=False),
                   'cpu_logical_cores': psutil.cpu_count(),
                   'total_physical_memory_gb': total_memory_gb,
                   'platform': platform.platform()
                }
            }

    return as_dict

def configure_for_extra_accuracy(config: PerfConfig, arguments: List[str]) -> List[str]:
    """
    When the `extra_accuracy` flag is set we want to run each test multiple times to 
    ensure a more stable result. However, this can take a lot of time for longer tests 
    so limit them to only a few minutes.
    """

    new_run_max = 5
    new_run_time="run_time=240"
    print("==================")
    print(f"Extra accuracy flag set. Overriding runmax to {new_run_max} and setting -o {new_run_time}")
    print("==================")
    
    config.run_max = new_run_max

    if arguments:
        for (i, arg) in enumerate(arguments):
            if arg.startswith("-o "):
                if "run_time=" in arg:
                    print("Error: Attempting to set `run_time` but it has already been set via the `-args` flag`")
                    exit(1)
                else:
                    arguments[i] = arg + "," + new_run_time
                    return
                    
    # There is no `-o` argument yet. Add one.
    if not(arguments):
        arguments = []
    arguments += [f"-o {new_run_time}"]
    return arguments

def run_test_wrapper(config: PerfConfig, index: int = 0, arguments: List[str] = None):
    for test_run in range(config.run_max):
        print("Starting test  {}".format(test_run))
        run_test(config=config, test_run=test_run, index=index, arguments=arguments)
        print("Completed test {}".format(test_run))


def run_test(config: PerfConfig, test_run: int, index: int = 0, arguments: List[str] = None):
    test_home = create_test_home_path(home=config.home_dir, test_run=test_run, index=index)
    if config.verbose:
        print("Home directory path created: {}".format(test_home))

    command_line = construct_command_line(
        exec_path=config.exec_path,
        arguments=arguments,
        test_arg=config.test_type.get_test_arg(config.test),
        home_arg=config.test_type.get_home_arg(test_home))

    try:
        proc = subprocess.run(command_line, check=True,
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT,
                              universal_newlines=True)
    except subprocess.CalledProcessError as cpe:
        print("Error: {}".format(cpe.output))
        exit(1)
    print(proc.stdout)
    with open("stdout_file.txt", 'w') as outfile:
        outfile.write(proc.stdout)


def process_results(config: PerfConfig, perf_stats: PerfStatCollection, index: int = 0) -> List[PerfStat]:
    for test_run in range(config.run_max):
        test_home = create_test_home_path(home=config.home_dir, test_run=test_run, index=index)
        if config.verbose:
            print('Reading stats from {} directory.'.format(test_home))
        perf_stats.find_stats(test_home=test_home)
    return perf_stats.to_report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()

    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--wtperf', action='store_true', help='run wtperf tests')
    group.add_argument('--workgen', action='store_true', help='run workgen tests')

    parser.add_argument('-e', '--exec_path', help='path of the test executable')
    parser.add_argument('-t', '--test', help='path of the test to execute')
    parser.add_argument('-o', '--outfile', help='path of the file to write test output to')
    parser.add_argument('-b', '--brief_output', action="store_true", help='brief (not detailed) test output')
    parser.add_argument('-m', '--runmax', type=int, default=1, help='maximum number of times to run the test')
    parser.add_argument('-ho', '--home', default="WT_TEST", help='path of the "home" directory that the test will use')
    parser.add_argument('-re',
                        '--reuse',
                        action="store_true",
                        help='reuse and reanalyse results from previous tests rather than running tests again')
    parser.add_argument('-bf', '--batch_file', help='Run all specified configurations for a single test')
    parser.add_argument('-args', '--arguments', help='Additional arguments to pass into the test')
    parser.add_argument('-ops', '--operations', help='List of operations to report metrics for')
    parser.add_argument('-v', '--verbose', action="store_true", help='be verbose')
    parser.add_argument('-a', '--improved_accuracy', action='store_true', help='Enable stable runs and results')
    args = parser.parse_args()

    if args.verbose:
        print('PerfPy')
        print('========')
        print("Configuration:")
        print("  Perf Exec:         {}".format(args.exec_path))
        print("  Test path:         {}".format(args.test))
        print("  Home base:         {}".format(args.home))
        print("  Batch file:        {}".format(args.batch_file))
        print("  Arguments:         {}".format(args.arguments))
        print("  Operations:        {}".format(args.operations))
        print("  Outfile:           {}".format(args.outfile))
        print("  Runmax:            {}".format(args.runmax))
        print("  Reuse results:     {}".format(args.reuse))
        print("  Brief output:      {}".format(args.brief_output))

    if args.exec_path is None:
        sys.exit('The path to the test executable is required')
    if args.test is None:
        sys.exit('The path to the test file is required')
    if args.batch_file and not os.path.isfile(args.batch_file):
        sys.exit("batch_file: {} not found!".format(args.batch_file))
    if args.batch_file and (args.arguments or args.operations):
        sys.exit("A batch file (-bf) should not be defined at the same time as -ops or -args")
    if not args.verbose and not args.outfile:
        sys.exit("Enable verbosity (or provide a file path) to dump the stats. "
                 "Try 'python3 perf_run.py --help' for more information.")

    return args


def parse_json_args(args: argparse.Namespace) -> Tuple[List[str], List[str], PerfConfig, Dict]:
    arguments = json.loads(args.arguments) if args.arguments else None
    operations = json.loads(args.operations) if args.operations else None
    test_type = TestType(is_wtperf=args.wtperf, is_workgen=args.workgen)

    config = PerfConfig(test_type=test_type,
                        exec_path=args.exec_path,
                        home_dir=args.home,
                        test=args.test,
                        batch_file=args.batch_file,
                        arguments=arguments,
                        operations=operations,
                        run_max=args.runmax,
                        verbose=args.verbose,
                        improved_accuracy=args.improved_accuracy)

    batch_file_contents = None
    if config.batch_file:
        if args.verbose:
            print("Reading batch file {}".format(config.batch_file))
        with open(config.batch_file, "r") as file:
            batch_file_contents = json.load(file)

    return (arguments, operations, config, batch_file_contents)


def validate_operations(config: PerfConfig, batch_file_contents: Dict, operations: List[str]):
    # Check for duplicate operations, and exit if duplicates are found
    # First, construct a list of all operations, including potential duplicates
    all_operations = []
    if config.batch_file:
        for content in batch_file_contents:
            all_operations += content["operations"]
    elif operations:
        all_operations += operations
    # Next, construct a new list with duplicates removed.
    # Converting to a dict and back is a simple way of doing this.
    all_operations_nodups = list(dict.fromkeys(all_operations))
    # Now check if any duplicate operations were removed in the deduplication step.
    if len(all_operations_nodups) != len(all_operations):
        sys.exit("List of all operations ({}) contains duplicates".format(all_operations))

    # Also check that all operations provided have an associated PerfStat.
    all_stat_names = [stat.short_label for stat in PerfStatCollection.all_stats()]
    for oper in all_operations:
        if oper not in all_stat_names:
            sys.exit(f"Provided operation '{oper}' does not match any known PerfStats.\n"
                     f"Possible names are: {sorted(all_stat_names)}")


def run_perf_tests(config: PerfConfig,
                   batch_file_contents: Dict,
                   args: argparse.Namespace,
                   arguments: List[str],
                   operations: List[str]) -> List[PerfStat]:
    reported_stats: List[PerfStat] = []

    if config.batch_file:
        if args.verbose:
            print("Batch tests to run: {}".format(len(batch_file_contents)))
        for content in batch_file_contents:
            index = batch_file_contents.index(content)
            if args.verbose:
                print("Batch test {}: Arguments: {}, Operations: {}".
                      format(index,  content["arguments"], content["operations"]))
            perf_stats = PerfStatCollection(content["operations"])
            if not args.reuse:
                if (config.improved_accuracy):
                    arguments = configure_for_extra_accuracy(config, content["arguments"])
                run_test_wrapper(config=config, index=index, arguments=content["arguments"])
            reported_stats += process_results(config, perf_stats, index=index)
    else:
        perf_stats = PerfStatCollection(operations)
        if not args.reuse:
            if (config.improved_accuracy):
                arguments = configure_for_extra_accuracy(config, arguments)
            run_test_wrapper(config=config, index=0, arguments=arguments)
        reported_stats = process_results(config, perf_stats)

    return reported_stats


def report_results(args: argparse.Namespace, config: PerfConfig, reported_stats: List[PerfStat]):
    if args.brief_output:
        if args.verbose:
            print("Brief stats output (Evergreen compatible format):")
        perf_results = brief_perf_stats(config, reported_stats)
    else:
        if args.verbose:
            print("Detailed stats output (Atlas compatible format):")
        perf_results = detailed_perf_stats(config, reported_stats)

    if args.verbose:
        perf_json = json.dumps(perf_results, indent=4, sort_keys=True)
        print("{}".format(perf_json))

    if args.outfile:
        dir_name = os.path.dirname(args.outfile)
        if dir_name:
            os.makedirs(dir_name, exist_ok=True)
        with open(args.outfile, 'w') as outfile:
            json.dump(perf_results, outfile, indent=4, sort_keys=True)


def main():
    args = parse_args()
    (arguments, operations, config, batch_file_contents) = parse_json_args(args=args)
    validate_operations(config=config, batch_file_contents=batch_file_contents, operations=operations)
    reported_stats = run_perf_tests(config=config,
                                    batch_file_contents=batch_file_contents,
                                    args=args,
                                    arguments=arguments,
                                    operations=operations)
    report_results(args=args, config=config, reported_stats=reported_stats)

if __name__ == '__main__':
    main()
