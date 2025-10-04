#!/usr/bin/env python
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
#
import csv
import logging
from pygit2 import Diff
from change_info import ChangeInfo


# is_useful_line detects lines of code that have 'useful' code in them.
# It is used to filter out displaying data, such as code coverage, for lines of code that don't do anything useful.
# This is used because gcov, for example, sometimes coverage reports counts for lines that only contain '}'
def is_useful_line(content: str) -> bool:
    useful_line = content != '\n' and content.strip() != '{' and content.strip() != '}'
    return useful_line


def diff_to_change_list(diff: Diff) -> dict:
    change_list = dict()
    for patch in diff:
        logging.debug('    {}: {}'.format(patch.delta.status_char(), patch.delta.new_file.path))
        if patch.delta.new_file.path != patch.delta.old_file.path:
            logging.debug('      (was {})'.format(patch.delta.old_file.path))

        hunks = patch.hunks
        hunk_list = list()
        for hunk in hunks:
            logging.debug('      Hunk:')
            logging.debug('        old_start: {}, old_lines: {}'.format(hunk.old_start, hunk.old_lines))
            logging.debug('        new_start: {}, new_lines: {}'.format(hunk.new_start, hunk.new_lines))

            change = ChangeInfo(status=patch.delta.status_char(),
                                new_file_path=patch.delta.new_file.path,
                                old_file_path=patch.delta.old_file.path,
                                new_start=hunk.new_start,
                                new_lines=hunk.new_lines,
                                old_start=hunk.old_start,
                                old_lines=hunk.old_lines,
                                lines=hunk.lines)
            hunk_list.append(change)

        change_list[patch.delta.new_file.path] = hunk_list

    return change_list


# read_complexity_data reads complexity data from a .csv file written by Metrix++ and returns as a list of dicts
def read_complexity_data(complexity_data_path: str) -> list:
    complexity_data = []
    with open(complexity_data_path) as csvfile:
        reader = csv.DictReader(csvfile)
        for row in reader:
            complexity_data.append(row)
    return complexity_data


# preprocess_complexity_data normalises file paths in complexity data and includes only functions in the output
def preprocess_complexity_data(complexity_data: list) -> dict:
    preprocessed_complexity_data = dict()

    for complexity_item in complexity_data:
        # Strip any (leading) './' and replace with 'src/'
        filename = complexity_item["file"].replace("./", "src/")
        complexity_item["file"] = filename

        if filename not in preprocessed_complexity_data:
            preprocessed_complexity_data[filename] = {}

        if complexity_item["type"] == "function":
            function_name = complexity_item["region"]
            preprocessed_complexity_data[filename][function_name] = complexity_item

    return preprocessed_complexity_data
