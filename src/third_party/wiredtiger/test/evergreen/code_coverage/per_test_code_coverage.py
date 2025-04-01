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

import argparse
import json
import logging
import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from code_coverage_utils import check_build_dirs, run_task_lists_in_parallel, setup_build_dirs, PushWorkingDirectory

# Clean up the run-time code coverage files ready to run another test
def delete_runtime_coverage_files(build_dir_base: str) -> None:
    for root, _, files in os.walk(build_dir_base):
        for filename in files:
            if filename.endswith('.gcda'):
                file_path = os.path.join(root, filename)
                os.remove(file_path)
                logging.debug(f"Deleted: {file_path}")


# Run a series of tests with code coverage, copying the results and cleaning up
# after each test is complete.
def run_coverage_task(index, task):
    env = os.environ.copy()
    build_dir = os.getcwd()
    # GCOV doesn't like it that we have copied the base build directory to construct the other
    # build directories. GCOV supports cross profiling for reference:
    # https://gcc.gnu.org/onlinedocs/gcc/Cross-profiling.html
    # The basic idea is that GCOV_PREFIX_STRIP, indicates how many directory path to strip away
    # from the absolute path, and the GCOV_PREFIX prepends the directory path. In this case,
    # we are stripping away /data/mci/commit_hash/wiredtiger/build and then applying the correct
    # build path.
    path_depth = build_dir.count("/")
    env["GCOV_PREFIX_STRIP"] = str(path_depth)
    env["GCOV_PREFIX"] = build_dir
    logging.debug("Running task {} in {}".format(task, build_dir))

    start_time = datetime.now()
    try:
        delete_runtime_coverage_files(build_dir_base=build_dir)
        p = PushWorkingDirectory(build_dir)
        split_command = task.split()
        subprocess.run(split_command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
        copy_dest_dir = f"{build_dir}_{index}_copy"
        logging.debug(f"Copying directory {build_dir} to {copy_dest_dir}")
        shutil.copytree(src=build_dir, dst=copy_dest_dir)

        task_info = {"task": task}
        task_info_as_json_object = json.dumps(task_info, indent=2)
        task_info_file_path = os.path.join(copy_dest_dir, "task_info.json")
        with open(task_info_file_path, "w") as output_file:
            output_file.write(task_info_as_json_object)
        p.pop()
    except subprocess.CalledProcessError as exception:
        print(f'Command {exception.cmd} failed with error {exception.returncode}')
    end_time = datetime.now()
    diff = end_time - start_time

    logging.debug("Finished task {} in {} : took {} seconds".format(task, build_dir, diff.total_seconds()))
    return 0


# Run gcovr on each copy of a build directory that contains run-time coverage data
def run_gcovr(build_dir_base: str, gcovr_dir: str):
    print(f"Starting run_gcovr({build_dir_base}, {gcovr_dir})")
    dir_name = os.path.dirname(build_dir_base)
    filenames_in_dir = os.listdir(dir_name)
    filenames_in_dir.sort()
    for file_name in filenames_in_dir:
        if file_name.startswith('build_') and file_name.endswith("copy"):
            build_copy_name = file_name
            build_copy_path = os.path.join(dir_name, build_copy_name)
            task_info_path = os.path.join(build_copy_path, "task_info.json")
            coverage_output_dir = os.path.join(gcovr_dir, build_copy_name)
            logging.debug(
                f"build_copy_name = {build_copy_name}, build_copy_path = {build_copy_path}, "
                f"task_info_path = {task_info_path}, coverage_output_dir = {coverage_output_dir}")
            os.mkdir(coverage_output_dir)
            shutil.copy(src=task_info_path, dst=coverage_output_dir)
            gcovr_command = (f"gcovr {build_copy_name} --gcov-ignore-parse-errors -f src -j 16 "
                             "--html-self-contained --html-details "
                             f"{coverage_output_dir}/2_coverage_report.html --json-summary-pretty "
                             f"--json-summary {coverage_output_dir}/1_coverage_report_summary.json "
                             f"--json {coverage_output_dir}/full_coverage_report.json")
            split_command = gcovr_command.split()
            env = os.environ.copy()
            logging.debug(f'env: {env}')
            logging.debug(f'gcovr_command: {gcovr_command}')
            try:
                completed_process = subprocess.run(split_command, env=env, check=True)
                output = completed_process.stdout
                print(f'Command returned {output}')
            except subprocess.CalledProcessError as exception:
                print(f'Command {exception.cmd} failed with error {exception.returncode} "{exception.output}"')
            logging.debug(f'Completed a run of gcovr on {build_copy_name}')
    print(f"Ending run_gcovr({build_dir_base}, {gcovr_dir})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--config_path', required=True, help='Path to the json config file')
    parser.add_argument('-b', '--build_dir_base', required=True, help='Base name for the build directories')
    parser.add_argument('-j', '--parallel', default=1, type=int, help='How many tests to run in parallel')
    parser.add_argument('-g', '--gcovr_dir', help='Directory to store gcovr output in')
    parser.add_argument('-s', '--setup', action="store_true",
                        help='Perform setup actions from the config in each build directory')
    parser.add_argument('-v', '--verbose', action="store_true", help='Be verbose')
    parser.add_argument('-e', '--check_errors', action="store_true", help='Check result codes from tasks and exit on failure')
    args = parser.parse_args()

    verbose = args.verbose
    config_path = args.config_path
    build_dir_base = args.build_dir_base
    gcovr_dir = args.gcovr_dir
    parallel_tests = args.parallel
    setup = args.setup
    check_errors = args.check_errors

    logging.basicConfig(level=logging.DEBUG if verbose else logging.INFO)

    logging.debug('Per-Test Code Coverage')
    logging.debug('======================')
    logging.debug('Configuration:')
    logging.debug(f'  Config file                      {config_path}')
    logging.debug(f'  Base name for build directories: {build_dir_base}')
    logging.debug(f'  Number of parallel tests:        {parallel_tests}')
    logging.debug(f'  Perform setup actions:           {setup}')
    logging.debug(f'  Gcovr output directory:          {gcovr_dir}')
    logging.debug(f'  Check errors:                    {check_errors}')

    if parallel_tests < 1:
        sys.exit("Number of parallel tests must be >= 1")

    # Load test config json file
    with open(config_path) as json_file:
        config = json.load(json_file)

    logging.debug('  Configuration:')
    logging.debug(config)

    if len(config['test_tasks']) < 1:
        sys.exit("No test tasks")

    setup_actions = config['setup_actions']

    if setup and len(setup_actions) < 1:
        sys.exit("No setup actions")

    logging.debug('  Setup actions: {}'.format(setup_actions))

    if gcovr_dir:
        if not Path(gcovr_dir).is_absolute():
            sys.exit("gcovr_dir must be an absolute path")

    build_dirs_list = []
    if setup:
        build_dirs_list = setup_build_dirs(build_dir_base=build_dir_base, parallel=parallel_tests, setup_task_list=config['setup_actions'])
    else:
        build_dirs_list = check_build_dirs(build_dir_base=build_dir_base, parallel=parallel_tests)

    # Prepare to run the tasks in the list
    task_list = list()
    for test_num in range(len(config['test_tasks'])):
        test = config['test_tasks'][test_num]
        logging.debug("Prepping test {} ".format(test))
        task_list.append(test)

    logging.debug("task_list: {}".format(task_list))

    # Perform code coverage task operations in parallel across the build directories
    run_task_lists_in_parallel(build_dirs_list, task_list, run_func=run_coverage_task, optimize_test_order=False, check_errors=check_errors)

    # Run gcovr if required
    if gcovr_dir:
        run_gcovr(build_dir_base=build_dir_base, gcovr_dir=gcovr_dir)


if __name__ == '__main__':
    main()
