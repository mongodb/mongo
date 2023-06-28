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
import glob
import json
import os
import re
from typing import List


class PerfStat:
    def __init__(self,
                 short_label: str,
                 output_label: str,
                 input_offset: int = 0,
                 output_precision: int = 0,
                 pattern: str = None,
                 stat_file: str = 'test.stat',
                 conversion_function=int):
        self.short_label: str = short_label
        self.output_label: str = output_label
        self.input_offset: int = input_offset
        self.output_precision: int = output_precision
        self.pattern: str = pattern
        self.stat_file = stat_file
        self.conversion_function = conversion_function
        self.values = []

    def add_values(self, values: list):
        for val in values:
            converted_value = self.conversion_function(val)
            self.values.append(converted_value)

    def find_stat(self, test_stat_path: str):
        matches = []
        for line in open(test_stat_path):
            match = re.search(self.pattern, line)
            if match:
                matches.append(float(line.split()[self.input_offset]))
        return matches

    def average(self, vals):
        return self.conversion_function(sum(vals) / len(vals))

    def get_value(self):
        """Return the average of all gathered values"""
        if len(self.values) >= 3:
            drop_min_and_max = sorted(self.values)[1:-1]
            return self.average(drop_min_and_max)
        else:
            return self.average(self.values)

    def get_value_list(self, brief: bool):
        as_dict = {
                    'name': self.output_label,
                    'value': self.get_value()
                }
        if not brief:
            as_dict['values'] = self.values
        return [as_dict]


class PerfStatMinMax(PerfStat):
    def get_value_list(self, brief: bool):
        avg_min_3_vals = self.average(sorted(self.values)[:3])
        avg_max_3_vals = self.average(sorted(self.values)[-3:])

        as_list = [
            {'name': f"Min {self.output_label}", 'value': avg_min_3_vals},
            {'name': f"Max {self.output_label}", 'value': avg_max_3_vals},
        ]

        if not brief:
            as_list.append({'name': f"All {self.output_label}s", 'values': sorted(self.values)})
        return as_list


class PerfStatCount(PerfStat):
    def find_stat(self, test_stat_path: str):
        """Return the total number of times a pattern matched"""
        total = 0
        test_stat_path = glob.glob(test_stat_path)[0]
        for line in open(test_stat_path):
            match = re.search(self.pattern, line)
            if match:
                total += 1
        return [total]


class PerfStatLatency(PerfStat):
    def __init__(self, short_label: str, stat_file: str, output_label: str, ops: List[str], num_max: int):
        super().__init__(short_label=short_label,
                         stat_file=stat_file,
                         output_label=output_label)
        self.num_max = num_max
        self.ops = ops

    def find_stat(self, test_stat_path: str):
        values = []
        for line in open(test_stat_path):
            as_dict = json.loads(line)
            for operation in self.ops:
                values.append(as_dict["wtperf"][operation]["max latency"])
        return values

    def get_value(self, nth_max: int):
        """Return the nth maximum number from all the gathered values"""
        return sorted(self.values)[-nth_max]

    def get_value_list(self, brief: bool):
        as_list = []
        num_max = min(len(self.values), self.num_max)
        for i in range(1, num_max + 1):
            as_dict = {
                'name': self.output_label + str(i),
                'value': self.get_value(i)
            }
            as_list.append(as_dict)
        if not brief:
            as_list.append({
                'name': "Latencies",
                'values': sorted(self.values)
            })
        return as_list


class PerfStatLatencyWorkgen(PerfStat):
    def get_value(self, nth_max: int):
        """Return the nth maximum number from all the gathered values"""
        return sorted(self.values)[-nth_max] if nth_max < len(self.values) else 0

    def get_value_list(self, brief: bool):
        as_list = []
        for i in range(1, 6):
            as_dict = {
                'name': self.output_label + str(i),
                'value': self.get_value(i)
            }
            as_list.append(as_dict)
        if not brief:
            as_list.append({
                'name': "Latencies",
                'values': sorted(self.values)
            })
        return as_list


class PerfStatDBSize(PerfStat):
    def find_stat(self, test_stat_path: str):
        home_path = os.path.dirname(test_stat_path)
        total_size = 0

        for ele in os.scandir(home_path):
            total_size += os.path.getsize(ele)

        return [total_size]
