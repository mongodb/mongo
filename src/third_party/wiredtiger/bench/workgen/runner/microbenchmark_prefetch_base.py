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

from runner import *
from workgen import *
from wiredtiger import stat

# This module contains common code used in performance benchmarking
# workloads with and without the pre-fetch functionality enabled.
class microbenchmark_prefetch:
    def __init__(self):
        self.context = Context()
        self.conn_config = "create,cache_size=1G,checkpoint=(wait=60,log_size=2GB),eviction=(threads_min=12,threads_max=12),session_max=600,statistics=(all),statistics_log=(wait=1,json=true,sources=[file:]),prefetch=(available=true,default=false)"
        self.conn = self.context.wiredtiger_open(self.conn_config)
        self.session = self.conn.open_session()
        self.nrows = 12000000
        self.workload = None

        table_config = "type=file"
        table_count = 1
        for i in range(0, table_count):
            table_name = "table:test_prefetch" + str(i)
            self.table = Table(table_name)
            self.session.create(table_name, table_config)
            self.table.options.key_size = 12
            self.table.options.value_size = 138

    def populate(self):
        print("Populating database...")
        pop_icount = self.nrows
        pop_ops = Operation(Operation.OP_INSERT, self.table)
        pop_thread = Thread(pop_ops * pop_icount)
        pop_workload = Workload(self.context, pop_thread)
        ret = pop_workload.run(self.conn)
        assert ret == 0, ret
        print("Finished populating database.")

    def print_prefetch_stats(self):
        stat_cursor = self.session.open_cursor('statistics:')
        blocks_read = stat_cursor[stat.conn.block_read][2]
        cache_read_app_count = stat_cursor[stat.conn.cache_read_app_count][2]
        print("blocks_read: %d" % blocks_read)
        print("cache_read_app_count: %d" % cache_read_app_count)

        # Write out the statistic to be plotted into a file.
        stat_filename = self.context.args.home + "/prefetch_stats.out"
        fh = open(stat_filename, 'w')
        fh.write("blocks read: %d" % blocks_read)
        fh.close()
        stat_cursor.close()
