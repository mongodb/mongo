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

import os
from perf_stat import PerfStat
from typing import List


def create_test_stat_path(test_home_path: str, test_stats_file: str):
    return os.path.join(test_home_path, test_stats_file)


class PerfStatCollection:
    def __init__(self):
        self.perf_stats = {}

    def add_stat(self, perf_stat: PerfStat):
        self.perf_stats[perf_stat.short_label] = perf_stat

    def find_stats(self, test_home: str, operations: List[str]):
        for stat in self.perf_stats.values():
            if not operations or stat.short_label in operations:
                test_stat_path = create_test_stat_path(test_home, stat.stat_file)
                values = stat.find_stat(test_stat_path=test_stat_path)
                stat.add_values(values=values)

    def to_value_list(self, brief: bool):
        stats_list = []
        for stat in self.perf_stats.values():
            if not stat.are_values_all_zero():
                stat_list = stat.get_value_list(brief = brief)
                stats_list.extend(stat_list)
        return stats_list
