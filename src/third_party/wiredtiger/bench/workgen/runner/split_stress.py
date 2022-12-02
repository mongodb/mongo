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

# A workload with small cache, small internal and leaf page sizes, faster splits
# and multiple threads inserting keys in random order. It stresses the page
# splits in order to catch split races.
#
from runner import *
from wiredtiger import *
from workgen import *

context = Context()
# Connection configuration.
conn_config = "cache_size=100MB,log=(enabled=false),statistics=[fast],statistics_log=(wait=1,json=false)"
conn = context.wiredtiger_open("create," + conn_config)
s = conn.open_session("")

# Table configuration.
table_config = "leaf_page_max=8k,internal_page_max=8k,leaf_key_max=1433,leaf_value_max=1433,type=file,memory_page_max=1MB,split_deepen_min_child=100"
tables = []
table_count = 3
for i in range(0, table_count):
    tname = "file:test" + str(i)
    table = Table(tname)
    s.create(tname, 'key_format=S,value_format=S,' + table_config)
    table.options.key_size = 64
    table.options.value_size = 200
    table.options.range = 100000000 # 100 million
    tables.append(table)

# Populate phase.
populate_threads = 1
icount = 50000
# There are multiple tables to be filled during populate,
# the icount is split between them all.
pop_ops = Operation(Operation.OP_INSERT, tables[0])
pop_ops = op_multi_table(pop_ops, tables)
nops_per_thread = icount // (populate_threads * table_count)
pop_thread = Thread(pop_ops * nops_per_thread)
pop_workload = Workload(context, populate_threads * pop_thread)
print('populate:')
ret = pop_workload.run(conn)
assert ret == 0, ret

# Run phase.
ops = Operation(Operation.OP_INSERT, tables[0])
ops = op_multi_table(ops, tables, False)
thread0 = Thread(ops)

workload = Workload(context, 20 * thread0)
workload.options.report_interval=5
workload.options.run_time=300
print('Split stress workload running...')
ret = workload.run(conn)
assert ret == 0, ret

latency_filename = context.args.home + "/latency.out"
latency.workload_latency(workload, latency_filename)
conn.close()
