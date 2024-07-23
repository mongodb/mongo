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
import sys
from datetime import datetime

class PushWorkingDirectory:
    def __init__(self, new_working_directory: str) -> None:
        self.original_working_directory = os.getcwd()
        self.new_working_directory = new_working_directory
        os.chdir(self.new_working_directory)

    def pop(self):
        os.chdir(self.original_working_directory)

# Execute each list of tasks in parallel
def run_task_lists_in_parallel(label, task_bucket_info, run_func):
    parallel = len(task_bucket_info)
    task_start_time = datetime.now()

    with concurrent.futures.ProcessPoolExecutor(max_workers=parallel) as executor:
        for e in executor.map(run_func, task_bucket_info):
             logging.debug(e)

    task_end_time = datetime.now()
    task_diff = task_end_time - task_start_time
    logging.debug("Time taken to perform {}: {} seconds".format(label, task_diff.total_seconds()))

# Check the relevant build directories exist and have the correct status if we are not setting up
def check_build_dirs(build_dir_base, parallel):
    task_bucket_info = list()

    for build_num in range(parallel):
        this_build_dir = "{}{}".format(build_dir_base, build_num)

        task_bucket_info.append({'build_dir': this_build_dir, 'task_bucket': []})

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

    logging.debug("Build dirs: {}".format(task_bucket_info))

    return task_bucket_info

# Create the relevant build directories to run tasks.
def setup_build_dirs(build_dir_base, parallel, setup_task_list):
    task_bucket_info = list()

    base_build_dir = "{}{}".format(build_dir_base, 0)
    if os.path.exists(base_build_dir):
        sys.exit('build directory exists within {}.'.format(base_build_dir))

    logging.debug('Creating build directory {}.'.format(base_build_dir))
    os.mkdir(base_build_dir)

    for build_num in range(parallel):
        this_build_dir = "{}{}".format(build_dir_base, build_num)
        task_bucket_info.append({'build_dir': this_build_dir, 'task_bucket': []})

    logging.debug("Build dirs: {}".format(task_bucket_info))

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
    for task_bucket in task_bucket_info[1:]:
        shutil.copytree(base_build_dir, task_bucket['build_dir'])
    return task_bucket_info


