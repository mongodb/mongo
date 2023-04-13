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

'''
Usage -
The input file should be a JSON file in the format {"key": 100, "key2": 200 ...}. 
The script will generate two output JSON files (Evergreen and Atlas compatible) based on this input data.
'''

import argparse
import json


def parse_input_file(input_file):
    ''' Read and parse the input JSON file '''
    with open(input_file, 'r') as f:
        input_data = json.load(f)
    return input_data


def generate_output_atlas(test_name, input_data):
    ''' Generate the atlas-compatible JSON file '''

    output_data = {
        "Test Name": test_name,
        "metrics": [
            {"name": key, "value": value}
            for key, value in input_data.items()
        ],
        "config": {}
    }
    return output_data


def generate_output_evg(test_name, input_data):
    ''' Generate the evg-compatible JSON file '''

    output_data = [{
        "info": {
            "test_name": test_name
        },
        "metrics": [
            {"name": key, "value": value}
            for key, value in input_data.items()
        ]
    }]
    return output_data


def main(args):
    input_data = parse_input_file(args.input_file)
    output_data_atlas = generate_output_atlas(args.test_name, input_data)
    output_data_evg = generate_output_evg(args.test_name, input_data)

    # Write the output JSON files
    with open(args.output_path + '/atlas_out_' + args.test_name + '.json', 'w+') as f:
        json.dump(output_data_atlas, f, indent=4, sort_keys=True)

    with open(args.output_path + '/evergreen_out_' + args.test_name + '.json', 'w+') as f:
        json.dump(output_data_evg, f, indent=4, sort_keys=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Generate two JSON files based on input data')
    parser.add_argument('-n', '--test_name', help='Name of the test', required=True)
    parser.add_argument('-i', '--input_file', help='Path to the input JSON file', required=True)
    parser.add_argument('-o', '--output_path', default=".", help='Path to the generated JSON files')
    args = parser.parse_args()
    main(args)
