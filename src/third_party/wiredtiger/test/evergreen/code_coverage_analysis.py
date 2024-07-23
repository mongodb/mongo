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
import os.path
import json
from collections import defaultdict

# This function reads a gcovr json summary file into a dict and returns it
def read_coverage_data(coverage_data_path: str):
    with open(coverage_data_path) as json_file:
        data = json.load(json_file)
        return data


# The timing data file contains two lines of text, each line containing a single integer.
# Line one contains the start time in seconds, line two contains the end time in seconds.
# This function reads the timing data and returns the difference, in seconds, between the start and end times.
def read_timing_data(timing_data_path: str):
    with open(timing_data_path) as file:
        line1 = file.readline()
        line2 = file.readline()
        start_time_secs = int(line1)
        end_time_secs = int(line2)
        delta_secs = end_time_secs - start_time_secs
        print("Timing data: {} to {}, delta {}".format(start_time_secs, end_time_secs, delta_secs))
        return delta_secs

# Calculate the branch coverage from the detailed files.
def get_branch_coverage(branch_coverage, coverage_files, outfile='atlas_out_code_coverage.json'):
    component_dict = defaultdict(list)
    new_component_dict = {}

    for i in coverage_files:
        component = i['filename'].split('/')[1]
        if component not in component_dict:
            component_dict[component] = [i['branch_covered'], i['branch_total']]
        else:
            component_dict[component][0] += i['branch_covered']
            component_dict[component][1] += i['branch_total']

    # Insert the overall branch coverage data
    new_component_dict['overall'] = branch_coverage

    for key in component_dict:
        percent =  100 * float(component_dict[key][0]) / float(component_dict[key][1])
        new_component_dict[key] = percent

    resultList = []
    for key, value in new_component_dict.items():
        atlas_format = {}
        atlas_format['name'] = key
        atlas_format['value']=value
        resultList.append(atlas_format)

    return (resultList)

# Generate the Atlas compatible format report.
def get_component_coverage(branch_coverage, coverage_files, outfile):
    atlas_format = {
                'Test Name': "Code Coverage",
                'config': {},
                'metrics': get_branch_coverage(branch_coverage, coverage_files, outfile),
            }

    dir_name = os.path.dirname(outfile)
    if dir_name:
        os.makedirs(dir_name, exist_ok=True)

    with open(outfile, 'w') as outfile:
        json.dump(atlas_format, outfile, indent=4, sort_keys=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-s', '--summary', required=True, help='Path to the gcovr json summary data file')
    parser.add_argument('-o', '--outfile', help='Path of the file to write test output to')
    parser.add_argument('-c', '--coverage_type',
                        help='Type of the coverage report to generate, component_coverage to generate component level coverage(by default the coverage type is overall)')
    parser.add_argument('-t', '--time', help='Path to the timing data file')
    parser.add_argument('-v', '--verbose', action="store_true", help='be verbose')
    args = parser.parse_args()

    if args.coverage_type is None:
        args.coverage_type='overall_coverage'

    if args.verbose:
        print('Code Coverage Analysis')
        print('======================')
        print('Configuration:')
        print('  Summary data file:  {}'.format(args.summary))
        print('  Output file:  {}'.format(args.outfile))
        print('  Timing data:    {}'.format(args.time))
        print('  Coverage type:    {}'.format(args.coverage_type))

    coverage_data = read_coverage_data(args.summary)
    branch_coverage = coverage_data['branch_percent']
    coverage_files = coverage_data['files']

    # Generate Atlas compatible format report.
    if args.coverage_type == 'component_coverage':
        get_component_coverage(branch_coverage, coverage_files, args.outfile)

    print("Branch coverage = {}%".format(branch_coverage))

    if (args.time):
        delta_secs = read_timing_data(args.time)
        delta_mins = delta_secs / 60.0
        code_coverage_per_min = branch_coverage / delta_mins
        print("Time taken: {} seconds".format(delta_secs))

        print("Code coverage rate = {:.2f}%/min".format(code_coverage_per_min))


if __name__ == '__main__':
    main()
