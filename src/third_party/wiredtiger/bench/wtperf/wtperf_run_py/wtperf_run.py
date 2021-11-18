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
from perf_stat import PerfStat, PerfStatCount, PerfStatLatency, PerfStatMax, PerfStatMin
from perf_stat_collection import PerfStatCollection
from pygit2 import discover_repository, Repository
from pygit2 import GIT_SORT_NONE
from typing import List
from wtperf_config import WTPerfConfig


def create_test_home_path(home: str, test_run: int, index:int):
    home_path = "{}_{}_{}".format(home, test_run, index)
    return home_path


def get_git_info(git_working_tree_dir):
    repository_path = discover_repository(git_working_tree_dir)
    assert repository_path is not None

    repo = Repository(repository_path)
    commits = list(repo.walk(repo.head.target, GIT_SORT_NONE))
    head_commit = commits[0]
    diff = repo.diff()

    git_info = {
        'head_commit': {
            'hash': head_commit.hex,
            'message': head_commit.message,
            'author': head_commit.author.name
            },
        'branch': {
            'name': repo.head.shorthand
        },
        'stats': {
            'files_changed': diff.stats.files_changed,
        },
        'num_commits': len(commits)
    }

    return git_info


def construct_wtperf_command_line(wtperf: str, env: str, test: str, home: str, arguments: List[str]):
    command_line = []
    if env is not None:
        command_line.append(env)
    command_line.append(wtperf)
    if test is not None:
        command_line.append('-O')
        command_line.append(test)
    if arguments is not None:
        command_line.extend(arguments)
    if home is not None:
        command_line.append('-h')
        command_line.append(home)
    return command_line


def brief_perf_stats(config: WTPerfConfig, perf_stats: PerfStatCollection):
    as_list = [{
        "info": {
            "test_name": os.path.basename(config.test)
        },
        "metrics": perf_stats.to_value_list(brief=True)
    }]
    return as_list


def detailed_perf_stats(config: WTPerfConfig, perf_stats: PerfStatCollection):
    total_memory_gb = psutil.virtual_memory().total / (1024 * 1024 * 1024)
    as_dict = {
                'Test Name': os.path.basename(config.test),
                'config': config.to_value_dict(),
                'metrics': perf_stats.to_value_list(brief=False),
                'system': {
                   'cpu_physical_cores': psutil.cpu_count(logical=False),
                   'cpu_logical_cores': psutil.cpu_count(),
                   'total_physical_memory_gb': total_memory_gb,
                   'platform': platform.platform()
                }
            }

    if config.git_root:
        as_dict['git'] = get_git_info(config.git_root)

    return as_dict


def run_test_wrapper(config: WTPerfConfig, index: int = 0, arguments: List[str] = None):
    for test_run in range(config.run_max):
        print("Starting test  {}".format(test_run))
        run_test(config=config, test_run=test_run, index=index, arguments=arguments)
        print("Completed test {}".format(test_run))


def run_test(config: WTPerfConfig, test_run: int, index: int = 0, arguments: List[str] = None):
    test_home = create_test_home_path(home=config.home_dir, test_run=test_run, index=index)
    if config.verbose:
        print("Home directory path created: {}".format(test_home))
    command_line = construct_wtperf_command_line(
        wtperf=config.wtperf_path,
        env=config.environment,
        arguments=arguments,
        test=config.test,
        home=test_home)
    try:
        subprocess.run(command_line, check=True, stderr=subprocess.STDOUT, stdout=subprocess.PIPE,
                       universal_newlines=True)
    except subprocess.CalledProcessError as cpe:
        print("Error: {}".format(cpe.output))
        exit(1)


def process_results(config: WTPerfConfig, perf_stats: PerfStatCollection, operations: List[str] = None, index: int = 0):
    for test_run in range(config.run_max):
        test_home = create_test_home_path(home=config.home_dir, test_run=test_run, index=index)
        if config.verbose:
            print('Reading stats from {} directory.'.format(test_home))
        perf_stats.find_stats(test_home=test_home, operations=operations)


def setup_perf_stats():
    perf_stats = PerfStatCollection()
    perf_stats.add_stat(PerfStat(short_label="load",
                                 pattern='Load time:',
                                 input_offset=2,
                                 output_label='Load time',
                                 output_precision=2,
                                 conversion_function=float))
    perf_stats.add_stat(PerfStat(short_label="insert",
                                 pattern=r'Executed \d+ insert operations',
                                 input_offset=1,
                                 output_label='Insert count'))
    perf_stats.add_stat(PerfStat(short_label="modify",
                                 pattern=r'Executed \d+ modify operations',
                                 input_offset=1,
                                 output_label='Modify count'))
    perf_stats.add_stat(PerfStat(short_label="read",
                                 pattern=r'Executed \d+ read operations',
                                 input_offset=1,
                                 output_label='Read count'))
    perf_stats.add_stat(PerfStat(short_label="truncate",
                                 pattern=r'Executed \d+ truncate operations',
                                 input_offset=1,
                                 output_label='Truncate count'))
    perf_stats.add_stat(PerfStat(short_label="update",
                                 pattern=r'Executed \d+ update operations',
                                 input_offset=1,
                                 output_label='Update count'))
    perf_stats.add_stat(PerfStat(short_label="checkpoint",
                                 pattern=r'Executed \d+ checkpoint operations',
                                 input_offset=1,
                                 output_label='Checkpoint count'))
    perf_stats.add_stat(PerfStatMax(short_label="max_update_throughput",
                                    pattern=r'updates,',
                                    input_offset=8,
                                    output_label='Max update throughput'))
    perf_stats.add_stat(PerfStatMin(short_label="min_update_throughput",
                                    pattern=r'updates,',
                                    input_offset=8,
                                    output_label='Min update throughput'))
    perf_stats.add_stat(PerfStatCount(short_label="warnings",
                                      pattern='WARN',
                                      output_label='Latency warnings'))
    perf_stats.add_stat(PerfStatLatency(short_label="top5_latencies_read_update",
                                        stat_file='monitor.json',
                                        output_label='Latency(read, update) Max',
                                        ops = ['read', 'update'],
                                        num_max = 5))
    perf_stats.add_stat(PerfStatCount(short_label="eviction_page_seen",
                                      stat_file='WiredTigerStat*',
                                      pattern='[0-9].wt cache: pages seen by eviction',
                                      output_label='Pages seen by eviction'))
    perf_stats.add_stat(PerfStatLatency(short_label="max_latency_insert",
                                        stat_file='monitor.json',
                                        output_label='Latency(insert) Max',
                                        ops = ['insert'],
                                        num_max = 1))
    perf_stats.add_stat(PerfStatLatency(short_label="max_latency_read_update",
                                        stat_file='monitor.json',
                                        output_label='Latency(read, update) Max',
                                        ops = ['read', 'update'],
                                        num_max = 1))
    perf_stats.add_stat(PerfStatMax(short_label="max_read_throughput",
                                    pattern=r'updates,',
                                    input_offset=4,
                                    output_label='Max read throughput'))
    perf_stats.add_stat(PerfStatMin(short_label="min_read_throughput",
                                    pattern=r'updates,',
                                    input_offset=4,
                                    output_label='Min read throughput'))
    return perf_stats


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--wtperf', help='path of the wtperf executable')
    parser.add_argument('-e', '--env', help='any environment variables that need to be set for running wtperf')
    parser.add_argument('-t', '--test', help='path of the wtperf test to execute')
    parser.add_argument('-o', '--outfile', help='path of the file to write test output to')
    parser.add_argument('-b', '--brief_output', action="store_true", help='brief (not detailed) test output')
    parser.add_argument('-m', '--runmax', type=int, default=1, help='maximum number of times to run the test')
    parser.add_argument('-ho', '--home', help='path of the "home" directory that wtperf will use')
    parser.add_argument('-re',
                        '--reuse',
                        action="store_true",
                        help='reuse and reanalyse results from previous tests rather than running tests again')
    parser.add_argument('-g', '--git_root', help='path of the Git working directory')
    parser.add_argument('-i', '--json_info', help='additional test information in a json format string')
    parser.add_argument('-bf', '--batch_file', help='Run all specified configurations for a single test')
    parser.add_argument('-args', '--arguments', help='Additional arguments to pass into wtperf')
    parser.add_argument('-ops', '--operations', help='List of operations to report metrics for')
    parser.add_argument('-v', '--verbose', action="store_true", help='be verbose')
    args = parser.parse_args()

    if args.verbose:
        print('WTPerfPy')
        print('========')
        print("Configuration:")
        print("  WtPerf path:       {}".format(args.wtperf))
        print("  Environment:       {}".format(args.env))
        print("  Test path:         {}".format(args.test))
        print("  Home base:         {}".format(args.home))
        print("  Batch file:        {}".format(args.batch_file))
        print("  Arguments:         {}".format(args.arguments))
        print("  Operations:        {}".format(args.operations))
        print("  Git root:          {}".format(args.git_root))
        print("  Outfile:           {}".format(args.outfile))
        print("  Runmax:            {}".format(args.runmax))
        print("  JSON info          {}".format(args.json_info))
        print("  Reuse results:     {}".format(args.reuse))
        print("  Brief output:      {}".format(args.brief_output))

    if args.wtperf is None:
        sys.exit('The path to the wtperf executable is required')
    if args.test is None:
        sys.exit('The path to the test file is required')
    if args.home is None:
        sys.exit('The path to the "home" directory is required')
    if args.batch_file and not os.path.isfile(args.batch_file):
        sys.exit("batch_file: {} not found!".format(args.batch_file))
    if args.batch_file and (args.arguments or args.operations):
        sys.exit("A batch file (-bf) should not be defined at the same time as -ops or -args")

    json_info = json.loads(args.json_info) if args.json_info else {}
    arguments = json.loads(args.arguments) if args.arguments else None
    operations = json.loads(args.operations) if args.operations else None

    config = WTPerfConfig(wtperf_path=args.wtperf,
                          home_dir=args.home,
                          test=args.test,
                          batch_file=args.batch_file,
                          arguments=arguments,
                          operations=operations,
                          environment=args.env,
                          run_max=args.runmax,
                          verbose=args.verbose,
                          git_root=args.git_root,
                          json_info=json_info)

    perf_stats: PerfStatCollection = setup_perf_stats()

    if config.batch_file:
        if args.verbose:
            print("Reading batch file {}".format(config.batch_file))
        with open(config.batch_file, "r") as file:
            batch_file_contents = json.load(file)

    # Run test
    if not args.reuse:
        if config.batch_file:
            for content in batch_file_contents:
                if args.verbose:
                    print("Argument: {},  Operation: {}".format(content["arguments"], content["operations"]))
                run_test_wrapper(config=config, index=batch_file_contents.index(content), arguments=content["arguments"])
        else:
            run_test_wrapper(config=config, arguments=arguments)

    if not args.verbose and not args.outfile:
        sys.exit("Enable verbosity (or provide a file path) to dump the stats. "
                 "Try 'python3 wtperf_run.py --help' for more information.")

    # Process result
    if config.batch_file:
        for content in batch_file_contents:
            process_results(config, perf_stats, operations=content["operations"], index=batch_file_contents.index(content))
    else:
        process_results(config, perf_stats, operations=operations)

    # Output result
    if args.brief_output:
        if args.verbose:
            print("Brief stats output (Evergreen compatible format):")
        perf_results = brief_perf_stats(config, perf_stats)
    else:
        if args.verbose:
            print("Detailed stats output (Atlas compatible format):")
        perf_results = detailed_perf_stats(config, perf_stats)

    if args.verbose:
        perf_json = json.dumps(perf_results, indent=4, sort_keys=True)
        print("{}".format(perf_json))

    if args.outfile:
        dir_name = os.path.dirname(args.outfile)
        if dir_name:
            os.makedirs(dir_name, exist_ok=True)
        with open(args.outfile, 'w') as outfile:
            json.dump(perf_results, outfile, indent=4, sort_keys=True)


if __name__ == '__main__':
    main()
