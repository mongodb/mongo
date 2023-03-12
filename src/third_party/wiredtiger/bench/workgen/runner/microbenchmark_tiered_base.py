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

# This is the tiered microbenchmark base class that has the common code needed
# for other workloads
#

import sys

from runner import *
from wiredtiger import *
from workgen import *
from wiredtiger import stat

class microbenchmark_tiered:

    def __init__(self, extension='dir_store'):
        self.storage_source = extension
        self.context = Context()
        tiered_storage_config = ",tiered_storage=(bucket=./bucket,bucket_prefix=pfx-,local_retention=0,name=dir_store)"
        tiered_storage_config += ",extensions=(./ext/storage_sources/dir_store/libwiredtiger_dir_store.so=(early_load=true))"
        conn_config = ",cache_size=2G,log=(enabled,file_max=10MB),statistics=(fast),statistics_log=(json,wait=1)"  # explicitly added
        conn_config += tiered_storage_config
        self.conn = self.context.wiredtiger_open("create," + conn_config, self.create_bucket)
        self.session = self.conn.open_session(None)
        wtperf_table_config = "key_format=S,value_format=S," +\
            "exclusive=true,allocation_size=4kb," +\
            "internal_page_max=64kb,leaf_page_max=4kb,split_pct=100,"
        self.table_config = "type=file"
        self.tables = []
        self.table_count = 4
        for i in range(0, self.table_count):
            tname = "table:test" + str(i)
            table = Table(tname)
            self.session.create(tname,wtperf_table_config + self.table_config)
            table.options.key_size = 20
            table.options.value_size = 100
            self.tables.append(table)

        ops = Operation(Operation.OP_SEARCH, self.tables[0])
        ops = op_multi_table(ops, self.tables, False)
        self.read_thread = Thread(ops)

        ops = Operation(Operation.OP_UPDATE, self.tables[0])
        ops = op_multi_table(ops, self.tables, False)
        self.update_thread = Thread(ops)

        ops = Operation(Operation.OP_INSERT, self.tables[0])
        ops = op_multi_table(ops, self.tables, False)
        self.insert_thread = Thread(ops)

    def populate(self):
        print ("Populate tables start")
        populate_threads = 5
        icount = 50000
        # There are multiple tables to be filled during populate,
        # the icount is split between them all.
        pop_ops = Operation(Operation.OP_INSERT, self.tables[0])
        pop_ops = op_multi_table(pop_ops, self.tables)
        nops_per_thread = icount // (populate_threads * self.table_count)
        pop_ops = op_group_transaction(pop_ops, 100, "")
        pop_thread = Thread(pop_ops * nops_per_thread)
        pop_workload = Workload(self.context, populate_threads * pop_thread)
        ret = pop_workload.run(self.conn)
        assert ret == 0, ret
        print ("Populate tables end")

    def create_bucket(self, home):
        bucket_path = home + "/" + "bucket"
        if not os.path.isdir(bucket_path):
            os.mkdir(bucket_path)

    def set_checkpoint_thread(self, ckpt_thread):
        self.checkpoint_thread = ckpt_thread

    def print_stats(self):
        stat_cursor = self.session.open_cursor('statistics:')
        flush_count = stat_cursor[stat.conn.flush_tier][2]
        stat_cursor.close()
        # With the timing for the test run and the frequency of flush_tier, we would expect 3 or more flush_tier calls.
        # We want that many to examine the impact of flush_tier response times.
        # If we don't see that many, it may be that individual flush_tier calls are taking a very long time.
        assert flush_count > 2, flush_count

    def run_workload(self):
        workload = Workload(self.context, 8 * self.read_thread + 2 * self.update_thread + 2 * self.insert_thread + self.checkpoint_thread)
        workload.options.run_time=300
        workload.options.report_interval=5
        workload.options.sample_interval_ms=1000
        workload.options.max_latency=1000
        ret = workload.run(self.conn)
        assert ret == 0, ret
        latency_filename = self.context.args.home + "/latency.out"
        latency.workload_latency(workload, latency_filename)

