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

import re
from perf_stat import PerfStat


def find_stat(test_stat_path: str, pattern: str, position_of_value: int):
    for line in open(test_stat_path):
        match = re.match(pattern, line)
        if match:
            return float(line.split()[position_of_value])
    return 0


class PerfStatCollection:
    def __init__(self):
        self.perf_stats = {}

    def add_stat(self, perf_stat: PerfStat):
        self.perf_stats[perf_stat.short_label] = perf_stat

    def find_stats(self, test_stat_path: str, operation: str):
        for stat in self.perf_stats.values():
            if operation is None or stat.short_label == operation:
                value = find_stat(test_stat_path=test_stat_path,
                                pattern=stat.pattern,
                                position_of_value=stat.input_offset)
                stat.add_value(value=value)

    def to_value_list(self, brief: bool):
        as_list = []
        for stat in self.perf_stats.values():
            if not stat.are_values_all_zero():
                as_dict = {
                    'name': stat.output_label,
                    'value': stat.get_core_average()
                }
                if not brief:
                    as_dict['values'] = stat.values
                as_list.append(as_dict)
        return as_list
