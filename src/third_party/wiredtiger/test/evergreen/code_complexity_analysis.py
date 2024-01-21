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
import ast
import csv
import json
import os.path
import pandas as pd
import sys

from collections import defaultdict
from heapq import nlargest

def get_atlas_compatible_code_statistics(summaryFile, dataFile, outfile='atlas_code_complexity.json'):
    ''' Generate the Atlas compatible format report. '''

    atlasFormat = {
                'Test Name': "Code Complexity",
                'config': {},
                'metrics': get_code_complexity(summaryFile, dataFile)
            }

    dirName = os.path.dirname(outfile)
    if dirName:
        os.makedirs(dirName, exist_ok=True)

    with open(outfile, 'w') as outfile:
        json.dump(atlasFormat, outfile, indent=4)

def get_code_complexity(summaryFile, dataFile):
    ''' Generate a list of intended contents from the complexity analysis. '''

    dataFrame = pd.read_csv(dataFile)
    numberOfRegions = 5
    resultList = []
    complexityDict = {}
    complexityDict['Average'] = get_average(summaryFile)
    complexityDict['Stats Ranges'] = get_complexity_ranges_list(dataFile, dataFrame)
    complexityDict['Top ' + str(numberOfRegions) + ' Regions'] = get_region_list(dataFile, dataFrame, numberOfRegions)

    resultList.append(complexityDict)
    return (resultList)

def get_region_list(dataFile, dataFrame, numberOfRegions):
    ''' Retrieve numberOfRegions/functions with the highest cyclomatic complexity values. '''

    top5 = dataFrame.nlargest(numberOfRegions, 'std.code.complexity:cyclomatic')
    atlasFormat = {}
    for index, row in top5.iterrows():
        atlasFormat[row["region"]] = row["std.code.complexity:cyclomatic"]

    return (atlasFormat)

def get_complexity_ranges_list(dataFile, dataFrame):
    ''' Retrieve 3 set of cyclomatic complexity ranges, above 20, above 50 and above 90. '''

    rangesList = [20, 50, 90]
    atlasFormat = {}
    for i in rangesList:
        aboveRangeString = 'Above ' + str(i)
        atlasFormat[aboveRangeString] = get_complexity_max_limit(dataFrame, i)

    return (atlasFormat)

def get_complexity_max_limit(dataFrame, max_limit):
    ''' Retrieve the number of cyclomatic complexity values above the max_limit. '''

    columnName = 'std.code.complexity:cyclomatic'
    # Select column 'std.code.complexity:cyclomatic' from the dataframe
    column = dataFrame[columnName]
    # Get count of values greater than max_limit in the column 'std.code.complexity:cyclomatic'
    count = column[column > max_limit].count()

    # Pandas aggregation functions (like sum, count and mean) returns a NumPy int64 type number not a Python integer.
    # Object of type int64 is not JSON serializable so need to convert into an int.

    return (int(count))

def get_average(complexity_summary_file):
    """
    Retrieve the cyclomatic code complexity value

    Below is the format of complexity_summary_file, we would extract the "avg" value of std.code.complexity.
    {
        "view": [
            {
                "data": {
                    "info": {"path": "./", "id": 1},
                    "aggregated-data": {
                        "std.code.complexity": {
                            "cyclomatic": {
                                "max": 91,
                                "min": 0,
                                "avg": 4.942372881355932,
                                "total": 14580.0,
                                "count": 2950,
                                "nonzero": False,
                                "distribution-bars": [ ],
                                "sup": 0,
                            }
                        },
                        "std.code.lines": {
                            "code": { }
                        },
                    },
                    "file-data": {},
                    "subdirs": [ ],
                    "subfiles": [ ],
                }
            }
        ]
    }
    """

    with open(complexity_summary_file, 'r') as f:
        data = f.read()

    d = ast.literal_eval(data)
    try:
        newDictionary = d["view"]
        average = newDictionary[0]['data']['aggregated-data']['std.code.complexity']['cyclomatic']['avg']
    except Exception as e:
        sys.exit(f"The summary file '{complexity_summary_file}' does not have view list or cyclomatic code complexity average value. "
                 "Please make sure the summary file is properly formatted {e.text}")

    return (average)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-s', '--summary', required=True, help='Path of the complexity summary file in the json format')
    parser.add_argument('-o', '--outfile', help='Path of the file to write analysis output to')
    parser.add_argument('-d', '--data_file', help='Code complexity data file in the csv format')

    args = parser.parse_args()
    get_atlas_compatible_code_statistics(args.summary, args.data_file, args.outfile)

if __name__ == '__main__':
    main()
