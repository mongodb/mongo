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
from perf_stat import PerfStat, PerfStatCount, PerfStatLatency, PerfStatMax, PerfStatMin
from typing import List


def create_test_stat_path(test_home_path: str, test_stats_file: str):
    return os.path.join(test_home_path, test_stats_file)


class PerfStatCollection:
    def __init__(self, operations: List[str]):
        self.to_report: List[PerfStat] = []
        if operations:
            self.to_report = [stat for stat in PerfStatCollection.all_stats() if stat.short_label in operations]

    def find_stats(self, test_home: str):
        for stat in self.to_report:
            test_stat_path = create_test_stat_path(test_home, stat.stat_file)
            values = stat.find_stat(test_stat_path=test_stat_path)
            stat.add_values(values=values)

    @staticmethod
    def all_stats():
        return [
            PerfStat(short_label="load",
                        pattern='Load time:',
                        input_offset=2,
                        output_label='Load time',
                        output_precision=2,
                        conversion_function=float),
            PerfStat(short_label="insert",
                        pattern=r'Executed \d+ insert operations',
                        input_offset=1,
                        output_label='Insert count'),
            PerfStat(short_label="modify",
                        pattern=r'Executed \d+ modify operations',
                        input_offset=1,
                        output_label='Modify count'),
            PerfStat(short_label="read",
                        pattern=r'Executed \d+ read operations',
                        input_offset=1,
                        output_label='Read count'),
            PerfStat(short_label="truncate",
                        pattern=r'Executed \d+ truncate operations',
                        input_offset=1,
                        output_label='Truncate count'),
            PerfStat(short_label="update",
                        pattern=r'Executed \d+ update operations',
                        input_offset=1,
                        output_label='Update count'),
            PerfStat(short_label="checkpoint",
                        pattern=r'Executed \d+ checkpoint operations',
                        input_offset=1,
                        output_label='Checkpoint count'),
            PerfStatMax(short_label="max_update_throughput",
                        pattern=r'updates,',
                        input_offset=8,
                        output_label='Max update throughput'),
            PerfStatMin(short_label="min_update_throughput",
                        pattern=r'updates,',
                        input_offset=8,
                        output_label='Min update throughput'),
            PerfStatCount(short_label="warnings",
                            pattern='WARN',
                            output_label='Latency warnings'),
            PerfStatLatency(short_label="top5_latencies_read_update",
                            stat_file='monitor.json',
                            output_label='Latency(read, update) Max',
                            ops = ['read', 'update'],
                            num_max = 5),
            PerfStatCount(short_label="eviction_page_seen",
                            stat_file='WiredTigerStat*',
                            pattern='[0-9].wt cache: pages seen by eviction',
                            output_label='Pages seen by eviction'),
            PerfStatLatency(short_label="max_latency_insert",
                            stat_file='monitor.json',
                            output_label='Latency(insert) Max',
                            ops = ['insert'],
                            num_max = 1),
            PerfStatLatency(short_label="max_latency_read_update",
                            stat_file='monitor.json',
                            output_label='Latency(read, update) Max',
                            ops = ['read', 'update'],
                            num_max = 1),
            PerfStatMax(short_label="max_read_throughput",
                        pattern=r'updates,',
                        input_offset=4,
                        output_label='Max read throughput'),
            PerfStatMin(short_label="min_read_throughput",
                        pattern=r'updates,',
                        input_offset=4,
                        output_label='Min read throughput')
        ]
