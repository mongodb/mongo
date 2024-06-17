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
from pygit2 import Diff
from code_change_helpers import diff_to_change_list, read_complexity_data, preprocess_complexity_data


# Read a json file into a dict
def read_json(json_file_path: str) -> dict:
    with open(json_file_path) as json_file:
        return json.load(json_file)


# Collate the code coverage data from a directory full of build directory copies
# into a dict. Be careful with memory usage as the resulting dict may be large.
def collate_coverage_data(gcovr_dir: str) -> dict:
    filenames_in_dir = os.listdir(gcovr_dir)
    filenames_in_dir.sort()
    logging.debug(f"Starting collate_coverage_data({gcovr_dir})")
    logging.debug(f"filenames_in_dir {filenames_in_dir}")
    collated_coverage_data = {}
    for file_name in filenames_in_dir:
        if file_name.startswith('build_') and file_name.endswith("copy"):
            build_coverage_name = file_name
            build_coverage_path = os.path.join(gcovr_dir, build_coverage_name)
            task_info_path = os.path.join(build_coverage_path, "task_info.json")
            full_coverage_path = os.path.join(build_coverage_path, "full_coverage_report.json")
            logging.debug(f"task_info_path = {task_info_path}, full_coverage_path = {full_coverage_path}")
            task_info = read_json(json_file_path=task_info_path)
            coverage_info = read_json(json_file_path=full_coverage_path)
            task = task_info["task"]
            dict_entry = {
                "task": task,
                "build_coverage_name": build_coverage_name,
                "full_coverage": coverage_info
            }
            collated_coverage_data[task] = dict_entry
    logging.debug(f"Ending collate_coverage_data({gcovr_dir})")
    return collated_coverage_data


# Get source code location information about each function from code complexity data
def get_function_info(file_path: str,
                      line_number: int,
                      preprocessed_complexity_data: dict):
    function_info = dict()

    if file_path in preprocessed_complexity_data:
        file_complexity_detail = preprocessed_complexity_data[file_path]

        for function_name in file_complexity_detail:
            complexity_detail = file_complexity_detail[function_name]
            detail_file = complexity_detail['file']
            detail_start_line_number = int(complexity_detail['line start'])
            detail_end_line_number = int(complexity_detail['line end'])
            if detail_file == file_path and detail_start_line_number <= line_number <= detail_end_line_number:
                function_info['name'] = complexity_detail['region']
                function_info['line start'] = detail_start_line_number
                function_info['line end'] = detail_end_line_number
                break

    return function_info


# Collate the code changes with the code location info from the complexity report to identify the changed functions
# and their locations.
def create_report_info(change_list: dict,
                       preprocessed_complexity_data: dict) -> dict:
    changed_function_info = dict()
    file_change_list = dict()

    for new_file in change_list:
        this_patch = change_list[new_file]
        change_info_list = list()
        for hunk in this_patch:
            change_info = {'status': hunk.status, 'new_start': hunk.new_start, 'new_lines': hunk.new_lines,
                           'old_start': hunk.old_start, 'old_lines': hunk.old_lines}
            lines_info = list()
            for line in hunk.lines:
                line_info = {'content': line.content, 'new_lineno': line.new_lineno, 'old_lineno': line.old_lineno}
                if line.new_lineno > 0:
                    # Added lines of code don't have a 'old_lineno' value (ie the value will be < 0).
                    # Changed lines of code appear as two entries: (1) a deleted line and (2) an added line.
                    # This means that added or changed lines of code will have an 'old_lineno' < 0.
                    if line.old_lineno < 0:

                        if preprocessed_complexity_data:
                            function_info = get_function_info(file_path=new_file,
                                                              line_number=line.new_lineno,
                                                              preprocessed_complexity_data=preprocessed_complexity_data)
                            if function_info:
                                if new_file not in changed_function_info:
                                    changed_function_info[new_file] = dict()
                                function_name = function_info['name']
                                changed_function_info[new_file][function_name] = function_info

                lines_info.append(line_info)

            change_info['lines'] = lines_info
            change_info_list.append(change_info)

            file_change_list[new_file] = change_info_list

    report = {'change_info_list': file_change_list, 'changed_functions': changed_function_info}

    return report


# Report on which tests reach a particular changed function
def get_function_coverage(coverage_data: dict, changed_function: str, file_name: str, start_line: int, end_line: int):
    for test in coverage_data:
        test_info = coverage_data[test]
        file_coverage_data = test_info['full_coverage']['files']
        for file_info in file_coverage_data:
            if file_name == file_info['file']:
                code_is_reached = False
                for line_info in file_info['lines']:
                    line_number = int(line_info['line_number'])
                    line_count = int(line_info['count'])
                    if start_line <= line_number <= end_line and line_count > 0:
                        code_is_reached = True
                if code_is_reached:
                    logging.debug(f"        Reached by test: {test}")


# Generate a report to stdout listing changed functions and which tests reach those functions
def generate_report(coverage_data: dict, change_list: dict, preprocessed_complexity_data: dict):
    logging.debug("Generating report...")
    report_info = create_report_info(change_list=change_list, preprocessed_complexity_data=preprocessed_complexity_data)
    for changed_file in report_info['changed_functions']:
        logging.debug(f'  Changed file: {changed_file}')
        for changed_function in report_info['changed_functions'][changed_file]:
            logging.debug(f'    Changed function: {changed_function}')
            function_info = report_info['changed_functions'][changed_file][changed_function]
            start_line = function_info['line start']
            end_line = function_info['line end']
            get_function_coverage(coverage_data=coverage_data, changed_function=changed_function,
                                  file_name=changed_file, start_line=start_line, end_line=end_line)
    logging.debug("Generated report")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-c', '--coverage_data_path', required=True, help='Path to the coverage data directory.')
    parser.add_argument('-d', '--git_diff_file', required=True, help='Path to a Git diff file')
    parser.add_argument('-m', '--metrix_complexity_data', required=True,
                        help='Path to the Metrix++ complexity data csv file')
    parser.add_argument('-v', '--verbose', action="store_true", help='Be verbose')
    args = parser.parse_args()

    verbose = args.verbose
    logging.basicConfig(level=logging.DEBUG if verbose else logging.INFO)
    coverage_data_path = args.coverage_data_path
    git_diff_file = args.git_diff_file
    complexity_data_file = args.metrix_complexity_data

    logging.debug('Per-Test Code Coverage Report')
    logging.debug('=============================')
    logging.debug('Configuration:')
    logging.debug(f'  Coverage data path:   {coverage_data_path}')
    logging.debug(f'  Complexity data file: {complexity_data_file}')
    logging.debug(f'  Git diff file path:   {git_diff_file}')
    logging.debug("")

    with open(git_diff_file, "r") as diff_file:
        diff_data = diff_file.read()
        diff = Diff.parse_diff(diff_data)
        change_list = diff_to_change_list(diff=diff)

        complexity_data = read_complexity_data(complexity_data_file)
        preprocessed_complexity_data = preprocess_complexity_data(complexity_data=complexity_data)

        coverage_data = collate_coverage_data(gcovr_dir=coverage_data_path)

        generate_report(coverage_data=coverage_data, change_list=change_list,
                        preprocessed_complexity_data=preprocessed_complexity_data)


if __name__ == '__main__':
    main()
