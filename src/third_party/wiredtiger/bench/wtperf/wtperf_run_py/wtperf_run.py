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
import json
import os.path
import re
import subprocess
import sys
import platform
import psutil

from wtperf_config import WTPerfConfig
from perf_stat import PerfStat
from perf_stat_collection import PerfStatCollection

# the 'test.stat' file is where wt-perf.c writes out it's statistics
# (within the directory specified by the 'home' parameter)
test_stats_file = 'test.stat'


def create_test_home_path(home: str, test_run: int):
    return '{}_{}'.format(home, test_run)


def create_test_stat_path(test_home_path: str):
    return os.path.join(test_home_path, test_stats_file)


def find_stat(test_stat_path: str, pattern: str, position_of_value: int):
    for line in open(test_stat_path):
        match = re.match(pattern, line)
        if match:
            return line.split()[position_of_value]
    return 0


def construct_wtperf_command_line(wtperf: str, env: str, test: str, home: str):
    command_line = []
    if env is not None:
        command_line.append(env)
    command_line.append(wtperf)
    if test is not None:
        command_line.append('-O')
        command_line.append(test)
    if home is not None:
        command_line.append('-h')
        command_line.append(home)
    return command_line


def brief_perf_stats(config: WTPerfConfig, perf_stats: PerfStatCollection):
    as_list = []
    as_list.append(
        {
            "info": {
                "test_name": os.path.basename(config.test)
            },
            "metrics": perf_stats.to_value_list(brief=True)
        }
    )
    return as_list


def detailed_perf_stats(config: WTPerfConfig, perf_stats: PerfStatCollection):
    total_memory_gb = psutil.virtual_memory().total / (1024 * 1024 * 1024)
    as_dict = {
                'config': config.to_value_dict(),
                'metrics': perf_stats.to_value_list(brief=False),
                'system': {
                   'cpu_physical_cores': psutil.cpu_count(logical=False),
                   'cpu_logical_cores': psutil.cpu_count(),
                   'total_physical_memory_gb': total_memory_gb,
                   'platform': platform.platform()
                }
            }
    return as_dict


def run_test(config: WTPerfConfig, test_run: int):
    test_home = create_test_home_path(home=config.home_dir, test_run=test_run)
    command_line = construct_wtperf_command_line(
        wtperf=config.wtperf_path,
        env=config.environment,
        test=config.test,
        home=test_home)
    # print('Command Line for test: {}'.format(command_line))
    subprocess.run(command_line)


def process_results(config: WTPerfConfig, perf_stats: PerfStatCollection):
    for test_run in range(config.run_max):
        test_home = create_test_home_path(home=config.home_dir, test_run=test_run)
        test_stats_path = create_test_stat_path(test_home)
        if config.verbose:
            print('Reading test stats file: {}'.format(test_stats_path))
        perf_stats.find_stats(test_stat_path=test_stats_path)


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
    return perf_stats


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--wtperf', help='path of the wtperf executable')
    parser.add_argument('-e', '--env', help='any environment variables that need to be set for running wtperf')
    parser.add_argument('-t', '--test', help='path of the wtperf test to execute')
    parser.add_argument('-o', '--outfile', help='path of the file to write test output to')
    parser.add_argument('-b', '--brief_output', action="store_true", help='brief(not detailed) test output')
    parser.add_argument('-m', '--runmax', type=int, default=1, help='maximum number of times to run the test')
    parser.add_argument('-ho', '--home', help='path of the "home" directory that wtperf will use')
    parser.add_argument('-re',
                        '--reuse',
                        action="store_true",
                        help='reuse and reanalyse results from previous tests rather than running tests again')
    parser.add_argument('-v', '--verbose', action="store_true", help='be verbose')
    args = parser.parse_args()

    if args.verbose:
        print('WTPerfPy')
        print('========')
        print("Configuration:")
        print("  WtPerf path:   {}".format(args.wtperf))
        print("  Environment:   {}".format(args.env))
        print("  Test path:     {}".format(args.test))
        print("  Home base:     {}".format(args.home))
        print("  Outfile:       {}".format(args.outfile))
        print("  Runmax:        {}".format(args.runmax))
        print("  Reuse results: {}".format(args.reuse))

    if args.wtperf is None:
        sys.exit('The path to the wtperf executable is required')
    if args.test is None:
        sys.exit('The path to the test file is required')
    if args.home is None:
        sys.exit('The path to the "home" directory is required')

    config = WTPerfConfig(wtperf_path=args.wtperf,
                          home_dir=args.home,
                          test=args.test,
                          environment=args.env,
                          run_max=args.runmax,
                          verbose=args.verbose)

    perf_stats: PerfStatCollection = setup_perf_stats()

    # Run tests (if we're not reusing results)
    if not args.reuse:
        for test_run in range(args.runmax):
            print("Starting test  {}".format(test_run))
            run_test(config=config, test_run=test_run)
            print("Completed test {}".format(test_run))

    if not args.verbose and not args.outfile:
        sys.exit("Enable verbosity (or provide a file path) to dump the stats. Try 'python3 wtperf_run.py --help' for more information.")

    process_results(config, perf_stats)

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
