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
import concurrent.futures
import json
import logging
import os
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path
from code_coverage_utils import check_build_dirs, run_task_lists_in_parallel


class PushWorkingDirectory:
    def __init__(self, new_working_directory: str) -> None:
        self.original_working_directory = os.getcwd()
        self.new_working_directory = new_working_directory
        os.chdir(self.new_working_directory)

    def pop(self):
        os.chdir(self.original_working_directory)


# Clean up the run-time code coverage files ready to run another test
def delete_runtime_coverage_files(build_dir_base: str) -> None:
    for root, dirs, files in os.walk(build_dir_base):
        for filename in files:
            if filename.endswith('.gcda'):
                file_path = os.path.join(root, filename)
                logging.debug(f"Deleting: {file_path}")
                os.remove(file_path)
                logging.debug(f"Deleted: {file_path}")


# Run a series of tests with code coverage, copying the results and cleaning up
# after each test is complete.
def run_coverage_task_list(task_list_info):
    build_dir = task_list_info["build_dir"]
    task_list = task_list_info["task_bucket"]
    list_start_time = datetime.now()
    for index in range(len(task_list)):
        task = task_list[index]
        logging.debug("Running task {} in {}".format(task, build_dir))

        start_time = datetime.now()
        try:
            delete_runtime_coverage_files(build_dir_base=build_dir)
            os.chdir(build_dir)
            split_command = task.split()
            subprocess.run(split_command, check=True)
            copy_dest_dir = f"{build_dir}_{index}_copy"
            logging.debug(f"Copying directory {build_dir} to {copy_dest_dir}")
            shutil.copytree(src=build_dir, dst=copy_dest_dir)

            task_info = {"task": task}
            task_info_as_json_object = json.dumps(task_info, indent=2)
            task_info_file_path = os.path.join(copy_dest_dir, "task_info.json")
            with open(task_info_file_path, "w") as output_file:
                output_file.write(task_info_as_json_object)

        except subprocess.CalledProcessError as exception:
            print(f'Command {exception.cmd} failed with error {exception.returncode}')
        end_time = datetime.now()
        diff = end_time - start_time

        logging.debug("Finished task {} in {} : took {} seconds".format(task, build_dir, diff.total_seconds()))

    list_end_time = datetime.now()
    diff = list_end_time - list_start_time

    return_value = "Completed task list in {} : took {} seconds".format(build_dir, diff.total_seconds())
    logging.debug(return_value)

    return return_value


# Execute each list of code coverage tasks in parallel
def run_coverage_task_lists_in_parallel(label, task_bucket_info):
    parallel = len(task_bucket_info)
    task_start_time = datetime.now()

    with concurrent.futures.ProcessPoolExecutor(max_workers=parallel) as executor:
        for e in executor.map(run_coverage_task_list, task_bucket_info):
            logging.debug(e)

    task_end_time = datetime.now()
    task_diff = task_end_time - task_start_time
    logging.debug("Time taken to perform {}: {} seconds".format(label, task_diff.total_seconds()))


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
            gcovr_command = (f"gcovr {build_copy_name} -f src -j 4 --html-self-contained --html-details "
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
    args = parser.parse_args()

    verbose = args.verbose
    config_path = args.config_path
    build_dir_base = args.build_dir_base
    gcovr_dir = args.gcovr_dir
    parallel_tests = args.parallel
    setup = args.setup

    logging.basicConfig(level=logging.DEBUG if verbose else logging.INFO)

    logging.debug('Per-Test Code Coverage')
    logging.debug('======================')
    logging.debug('Configuration:')
    logging.debug(f'  Config file                      {config_path}')
    logging.debug(f'  Base name for build directories: {build_dir_base}')
    logging.debug(f'  Number of parallel tests:        {parallel_tests}')
    logging.debug(f'  Perform setup actions:           {setup}')
    logging.debug(f'  Gcovr output directory:          {gcovr_dir}')

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

    build_dirs = check_build_dirs(build_dir_base, parallel_tests, setup)

    setup_bucket_info = []
    task_bucket_info = []
    for build_dir in build_dirs:
        if setup:
            if len(os.listdir(build_dir)) > 0:
                sys.exit("Directory {} is not empty".format(build_dir))
            setup_bucket_info.append({'build_dir': build_dir, 'task_bucket': config['setup_actions']})
        task_bucket_info.append({'build_dir': build_dir, 'task_bucket': []})

    if setup:
        # Perform setup operations
        run_task_lists_in_parallel(label="setup", task_bucket_info=setup_bucket_info)

    # Prepare to run the tasks in the list
    for test_num in range(len(config['test_tasks'])):
        test = config['test_tasks'][test_num]
        build_dir_number = test_num % parallel_tests
        logging.debug("Prepping test [{}] as build number {}: {} ".format(test_num, build_dir_number, test))
        task_bucket_info[build_dir_number]['task_bucket'].append(test)

    logging.debug("task_bucket_info: {}".format(task_bucket_info))

    # Perform code coverage task operations in parallel across the build directories
    run_coverage_task_lists_in_parallel(label="tasks", task_bucket_info=task_bucket_info)

    # Run gcovr if required
    if gcovr_dir:
        run_gcovr(build_dir_base=build_dir_base, gcovr_dir=gcovr_dir)


if __name__ == '__main__':
    main()
