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

# Generated from runner/read_write_heavy.wtperf originally, then hand edited.
# A long run demonstrating how read or write threads can be synchronized.
import sys

from runner import *
from wiredtiger import *
from workgen import *

context = Context()
conn_config = ""
conn_config += ",cache_size=2GB,eviction=(threads_max=8),log=(enabled=true),session_max=250,statistics=(fast),statistics_log=(wait=1,json)"   # explicitly added
conn = context.wiredtiger_open("create," + conn_config)
s = conn.open_session("")

wtperf_table_config = "key_format=S,value_format=S," +\
    "exclusive=true,allocation_size=4kb," +\
    "internal_page_max=64kb,leaf_page_max=4kb,split_pct=100,"
compress_table_config = "block_compressor=snappy,"
table_config = "memory_page_max=10m,leaf_value_max=64MB,checksum=on,split_pct=90,type=file,log=(enabled=false),leaf_page_max=32k,block_compressor=snappy"
tables = []
table_count = 100
for i in range(0, table_count):
    tname = "table:test" + str(i)
    table = Table(tname)
    s.create(tname, wtperf_table_config +\
             compress_table_config + table_config)
    table.options.key_size = 20
    table.options.value_size = 7000
    tables.append(table)

populate_threads = 4
icount = 4000000
# There are multiple tables to be filled during populate,
# the icount is split between them all.
pop_ops = Operation(Operation.OP_INSERT, tables[0])
pop_ops = op_multi_table(pop_ops, tables)
nops_per_thread = icount // (populate_threads * table_count)
pop_thread = Thread(pop_ops * nops_per_thread)
pop_workload = Workload(context, populate_threads * pop_thread)
ret = pop_workload.run(conn)
assert ret == 0, ret
print('populate complete')

# Log like file, requires that logging be enabled in the connection config.
log_name = "table:log"
s.create(log_name, wtperf_table_config + "key_format=S,value_format=S," + compress_table_config + table_config + ",log=(enabled=true)")
log_table = Table(log_name)

ops = Operation(Operation.OP_UPDATE, tables[0])
ops = op_multi_table(ops, tables, False)
ops = op_log_like(ops, log_table, 0)
thread0 = Thread(ops)
# These operations include log_like operations, which will increase the number
# of insert/update operations by a factor of 2.0. This may cause the
# actual operations performed to be above the throttle.
thread0.options.throttle=11
thread0.options.throttle_burst=0

ops = Operation(Operation.OP_SEARCH, tables[0])
ops = op_multi_table(ops, tables, False)
ops = op_log_like(ops, log_table, 0)
thread1 = Thread(ops)
thread1.options.throttle=60
thread1.options.throttle_burst=0

ops = Operation(Operation.OP_SLEEP, "60") + \
      Operation(Operation.OP_CHECKPOINT, "")
checkpoint_thread = Thread(ops)

ops = Operation(Operation.OP_SLEEP, "0.1") + \
      Operation(Operation.OP_LOG_FLUSH, "")
logging_thread = Thread(ops)

############################################################################
# This part was added to the generated file.
# Add unthrottled threads that are synchronized so we get two minutes with
# unthrottled readers + writers, two minutes of unthrottled writers, two minutes
# of unthrottled readers and two minutes of idle. That is repeated. This is
# against a background of light reading and writing by the throttled threads.

ops = Operation(Operation.OP_UPDATE, tables[0])
ops = op_multi_table(ops, tables, False)
ops = op_log_like(ops, log_table, 0)
ops = timed(240.0, ops) + sleep(240.0)
thread_big_writer = Thread(ops)
thread_big_writer.synchronized = True

ops = Operation(Operation.OP_SEARCH, tables[0])
ops = op_multi_table(ops, tables, False)
ops = op_log_like(ops, log_table, 0)
ops = timed(120.0, ops) + timed(240.0, ops) + sleep(120.0)
thread_big_reader = Thread(ops)
thread_big_reader.synchronized = True

# End of added section.
# The new threads will also be added to the workload below.
############################################################################

workload = Workload(context, 20 * thread0 + 20 * thread1 + checkpoint_thread + logging_thread + 50 * thread_big_writer + 50 * thread_big_reader)
workload.options.report_interval=1
workload.options.run_time=1800
workload.options.sample_rate=1
workload.options.warmup=0
workload.options.sample_interval_ms = 1000
ret = workload.run(conn)
assert ret == 0, ret

latency_filename = context.args.home + "/latency.out"
latency.workload_latency(workload, latency_filename)
