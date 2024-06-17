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
import subprocess
import sys
from datetime import datetime


def run_task_list(task_list_info):
    build_dir = task_list_info["build_dir"]
    task_list = task_list_info["task_bucket"]
    list_start_time = datetime.now()
    for task in task_list:
        logging.debug("Running task {} in {}".format(task, build_dir))

        start_time = datetime.now()
        try:
            os.chdir(build_dir)
            split_command = task.split()
            subprocess.run(split_command, check=True)
        except subprocess.CalledProcessError as exception:
            logging.error(f'Command {exception.cmd} failed with error {exception.returncode}')
        end_time = datetime.now()
        diff = end_time - start_time

        logging.debug("Finished task {} in {} : took {} seconds".format(task, build_dir, diff.total_seconds()))

    list_end_time = datetime.now()
    diff = list_end_time - list_start_time

    return_value = "Completed task list in {} : took {} seconds".format(build_dir, diff.total_seconds())
    logging.debug(return_value)

    return return_value


# Execute each list of tasks in parallel
def run_task_lists_in_parallel(label, task_bucket_info):
    parallel = len(task_bucket_info)
    task_start_time = datetime.now()

    with concurrent.futures.ProcessPoolExecutor(max_workers=parallel) as executor:
        for e in executor.map(run_task_list, task_bucket_info):
             logging.debug(e)

    task_end_time = datetime.now()
    task_diff = task_end_time - task_start_time
    logging.debug("Time taken to perform {}: {} seconds".format(label, task_diff.total_seconds()))


# Check the relevant build directories exist and have the correct status
def check_build_dirs(build_dir_base, parallel, setup):
    build_dirs = list()

    for build_num in range(parallel):
        this_build_dir = "{}{}".format(build_dir_base, build_num)

        if not os.path.exists(this_build_dir):
            sys.exit("Build directory {} doesn't exist".format(this_build_dir))

        build_dirs.append(this_build_dir)

        found_compile_time_coverage_files = False
        found_run_time_coverage_files = False

        # Check build dir for coverage files
        for root, dirs, files in os.walk(this_build_dir):
            for filename in files:
                if filename.endswith('.gcno'):
                    found_compile_time_coverage_files = True
                if filename.endswith('.gcda'):
                    found_run_time_coverage_files = True

        logging.debug('Found compile time coverage files in {} = {}'.
              format(this_build_dir, found_compile_time_coverage_files))
        logging.debug('Found run time coverage files in {}     = {}'.
              format(this_build_dir, found_run_time_coverage_files))

        if not setup and not found_compile_time_coverage_files:
            sys.exit('No compile time coverage files found within {}. Please build for code coverage.'
                     .format(this_build_dir))

    logging.debug("Build dirs: {}".format(build_dirs))

    return build_dirs


