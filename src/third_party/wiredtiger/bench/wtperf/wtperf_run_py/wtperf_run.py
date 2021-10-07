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

# example parameters: -p /Users/jeremy.thorp/Git/wiredtiger/build/bench/wtperf/wtperf -t ../runners/small-lsm.wtperf -v -ho WT_TEST -m 3

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

    total_memory_gb = psutil.virtual_memory().total / (1024 * 1024 * 1024)
    as_dict = {'config': config.to_value_dict(),
               'metrics': perf_stats.to_value_list(),
               'system': {
                   'cpu_physical_cores': psutil.cpu_count(logical=False),
                   'cpu_logical_cores': psutil.cpu_count(),
                   'total_physical_memory_gb': total_memory_gb,
                   'platform': platform.platform()}
               }
    return as_dict

def setup_perf_stats():
    perf_stats = PerfStatCollection()
    perf_stats.add_stat(PerfStat(short_label="load",
                                 pattern='Load time:',
                                 input_offset=2,
                                 output_label='Load time:',
                                 output_precision=2,
                                 conversion_function=float))
    perf_stats.add_stat(PerfStat(short_label="insert",
                                 pattern='Executed \d+ insert operations',
                                 input_offset=1,
                                 output_label='Insert count:'))
    perf_stats.add_stat(PerfStat(short_label="modify",
                                 pattern='Executed \d+ modify operations',
                                 input_offset=1,
                                 output_label='Modify count:'))
    perf_stats.add_stat(PerfStat(short_label="read",
                                 pattern='Executed \d+ read operations',
                                 input_offset=1,
                                 output_label='Read count:'))
    perf_stats.add_stat(PerfStat(short_label="truncate",
                                 pattern='Executed \d+ truncate operations',
                                 input_offset=1,
                                 output_label='Truncate count:'))
    perf_stats.add_stat(PerfStat(short_label="update",
                                 pattern='Executed \d+ update operations',
                                 input_offset=1,
                                 output_label='Update count:'))
    return perf_stats

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', '--wtperf', help='path of the wtperf executable')
    parser.add_argument('-e', '--env', help='any environment variables that need to be set for running wtperf')
    parser.add_argument('-t', '--test', help='path of the wtperf test to execute')
    parser.add_argument('-o', '--outfile', help='path of the file to write test output to')
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

    # Process results
    perf_dict = process_results(config, perf_stats)
    perf_json = json.dumps(perf_dict, indent=4, sort_keys=True)

    if args.verbose:
        print("JSON: {}".format(perf_json))

    if args.outfile:
        with open(args.outfile, 'w') as outfile:
            json.dump(perf_dict, outfile, indent=4, sort_keys=True)

if __name__ == '__main__':
    main()
