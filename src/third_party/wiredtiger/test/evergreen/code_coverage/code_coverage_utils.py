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

import concurrent.futures
import logging
import os
import shutil
import subprocess
import multiprocessing
import sys
from datetime import datetime

class PushWorkingDirectory:
    def __init__(self, new_working_directory: str) -> None:
        self.original_working_directory = os.getcwd()
        self.new_working_directory = new_working_directory
        os.chdir(self.new_working_directory)

    def pop(self):
        os.chdir(self.original_working_directory)

# Setup each process with their own build directory, using a shared queue mechanism
def setup_run_tasks_parallel(init_args):
    build_dir = init_args.get()
    os.chdir(build_dir)
    return 0

# Execute each list of tasks in parallel
def run_task_lists_in_parallel(build_dirs_list, task_list, run_func, optimize_test_order, check_errors):
    parallel = len(build_dirs_list)
    task_start_time = datetime.now()

    # Build a shared queue of all the build directories, which will be used to initialize each
    # process to its own build directory.
    build_queue = multiprocessing.Queue()
    for build_dir in build_dirs_list:
        build_queue.put(build_dir)

    analyse_test_timings = list()
    futures = list()
    with concurrent.futures.ProcessPoolExecutor(max_workers=parallel, initializer=setup_run_tasks_parallel, initargs=(build_queue,)) as executor:
        for index, task in enumerate(task_list):
            futures.append(executor.submit(run_func, index, task))

        if check_errors:
            # Check the results of each of the futures. Calling result() will throw an exception for any that failed.
            for future in concurrent.futures.as_completed(futures):
                future.result()

        # Only in analysis mode, do we construct a list of all the tasks and how long they
        # took to run
        if optimize_test_order:
            for future in concurrent.futures.as_completed(futures):
                data = future.result()
                analyse_test_timings.append(data)

    task_end_time = datetime.now()
    task_diff = task_end_time - task_start_time
    logging.debug("Time taken to perform tasks: {} seconds".format(task_diff.total_seconds()))
    return analyse_test_timings

# Check the relevant build directories exist and have the correct status if we are not setting up
def check_build_dirs(build_dir_base, parallel):
    build_dirs_list = list()

    for build_num in range(parallel):
        this_build_dir = "{}{}".format(build_dir_base, build_num)

        build_dirs_list.append(this_build_dir)

        # Check build dir for coverage files.
        found_compile_time_coverage_files = False
        found_run_time_coverage_files = False
        for _, _, files in os.walk(this_build_dir):
            for filename in files:
                if filename.endswith('.gcno'):
                    found_compile_time_coverage_files = True
                if filename.endswith('.gcda'):
                    found_run_time_coverage_files = True

        logging.debug('Found compile time coverage files in {} = {}'.
            format(this_build_dir, found_compile_time_coverage_files))
        logging.debug('Found run time coverage files in {}     = {}'.
            format(this_build_dir, found_run_time_coverage_files))

        if not found_compile_time_coverage_files:
            sys.exit('No compile time coverage files found within {}. Please build for code coverage.'
                    .format(this_build_dir))

    logging.debug("Build dirs: {}".format(build_dirs_list))

    return build_dirs_list

# Create the relevant build directories to run tasks.
def setup_build_dirs(build_dir_base, parallel, setup_task_list):
    build_dirs_list = list()

    base_build_dir = "{}{}".format(build_dir_base, 0)
    if os.path.exists(base_build_dir):
        sys.exit('build directory exists within {}.'.format(base_build_dir))

    logging.debug('Creating build directory {}.'.format(base_build_dir))
    os.mkdir(base_build_dir)

    for build_num in range(parallel):
        this_build_dir = "{}{}".format(build_dir_base, build_num)
        build_dirs_list.append(this_build_dir)

    logging.debug("Build dirs: {}".format(build_dirs_list))

    logging.debug("Compiling base build directory: {}".format(base_build_dir))
    start_time = datetime.now()
    for task in setup_task_list:
        try:
            p = PushWorkingDirectory(base_build_dir)
            split_command = task.split()
            subprocess.run(split_command, check=True, capture_output=True)
            p.pop()
        except subprocess.CalledProcessError as exception:
            logging.error(f'Command {exception.cmd} failed with error {exception.returncode}')
    end_time = datetime.now()
    diff = end_time - start_time

    logging.debug("Finished setup and took {} seconds".format(diff.total_seconds()))
    # Copy compiled base build directory into the other build directores.
    logging.debug("Copying base build directory {} into the other build directories.".format(base_build_dir))
    for build_dir in build_dirs_list[1:]:
        shutil.copytree(base_build_dir, build_dir)
    return build_dirs_list


