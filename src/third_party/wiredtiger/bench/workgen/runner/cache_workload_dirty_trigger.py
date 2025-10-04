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
# Create waves of extra read and write activity (storms).

from runner import *
from wiredtiger import *
from workgen import *

context = Context()
# eviction_updates_trigger=30
conn_config = ""
conn_config += ",cache_size=10GB,eviction=(threads_max=8),log=(enabled=true),session_max=250,statistics=(fast),statistics_log=(wait=1,json=true),io_capacity=(total=30M)"   # explicitly added
conn = context.wiredtiger_open("create," + conn_config)
s = conn.open_session("")

wtperf_table_config = "key_format=S,value_format=S,exclusive=true,allocation_size=4kb,internal_page_max=64kb,"
table_config = "memory_page_max=10m,leaf_value_max=64MB,checksum=on,split_pct=90,type=file,log=(enabled=false),leaf_page_max=32k"
tables = []
table_count = 10
# Configure key and value sizes.
cfg_key_size = 10
cfg_value_size = 2000
for i in range(0, table_count):
    tname = "table:test" + str(i)
    table = Table(tname)
    s.create(tname, wtperf_table_config + table_config)
    table.options.key_size = cfg_key_size
    table.options.value_size = cfg_value_size
    tables.append(table)

populate_threads = 4
icount = 1000000
# There are multiple tables to be filled during populate,
# the icount is split between them all.
pop_ops = Operation(Operation.OP_INSERT, tables[0])
pop_ops = op_multi_table(pop_ops, tables)
nops_per_thread = icount // (populate_threads * table_count)
pop_thread = Thread(pop_ops * nops_per_thread)
pop_workload = Workload(context, populate_threads * pop_thread)
ret = pop_workload.run(conn)
assert ret == 0, ret

print('Populate complete')

# Log like file, requires that logging be enabled in the connection config.
log_name = "table:log"
s.create(log_name, wtperf_table_config + table_config + ",log=(enabled=true)")
log_table = Table(log_name)
log_table.options.key_size = cfg_key_size
log_table.options.value_size = cfg_value_size

ops = Operation(Operation.OP_UPDATE, log_table)
ops = op_log_like(ops, log_table, 0)
thread_log_upd = Thread(ops)
thread_log_upd.options.session_config="isolation=snapshot"
# These operations include log_like operations, which will increase the number
# of insert/update operations by a factor of 2.0. This may cause the
# actual operations performed to be above the throttle.
thread_log_upd.options.throttle=11
thread_log_upd.options.throttle_burst=0

ops = Operation(Operation.OP_SEARCH, log_table)
ops = op_log_like(ops, log_table, 0)
thread_log_read = Thread(ops)
thread_log_read.options.session_config="isolation=snapshot"
thread_log_read.options.throttle=60
thread_log_read.options.throttle_burst=0

############################################################################
# This part was added to the generated file.
# Add threads that do a bunch of operations and sleep, all in a loop.
# At the beginning of the run the threads will tend to be synchronized,
# but that effect will dissipate over time.

# This workload is to hit the update trigger
ops = Operation(Operation.OP_UPDATE, tables[0])
ops = txn(ops, 'isolation=snapshot')
# use_commit_timestamp - Commit the transaction with commit_timestamp.
ops.transaction.use_commit_timestamp = True
ops = op_multi_table(ops, tables, False)
ops = ops * 10000 + Operation(Operation.OP_SLEEP, "10")
thread_upd10k_sleep10 = Thread(ops)
thread_upd10k_sleep10.options.name = "Update"
thread_upd10k_sleep10.options.session_config="isolation=snapshot"

ops = Operation(Operation.OP_SEARCH, tables[0])
ops = txn(ops, 'isolation=snapshot')
# use_commit_timestamp - Commit the transaction with commit_timestamp.
ops.transaction.use_commit_timestamp = True
ops = op_multi_table(ops, tables, False)
ops = ops * 10000 + Operation(Operation.OP_SLEEP, "10")
thread_read10k_sleep10 = Thread(ops)
thread_upd10k_sleep10.options.name = "Search"
thread_read10k_sleep10.options.session_config="isolation=snapshot"

# End of added section.
# The new threads will also be added to the workload below.
############################################################################

#Configure only the expected % of read operations
#15% update workload to hit the cache dirty trigger threshold
read_ops = 85

#Specify number of threads for workload
total_thread_num = 128
log_read_thread_num = 1
log_write_thread_num = 1
#Calculate threads for read operations based on % of read operations
read_thread_num = int(((read_ops * total_thread_num) / 100))
#Remaining threads for write operations
write_thread_num = (total_thread_num - read_thread_num)

cache_workload = Workload(context,\
                        + log_write_thread_num * thread_log_upd \
                        + log_read_thread_num * thread_log_read \
                        + write_thread_num * thread_upd10k_sleep10 \
                        + read_thread_num * thread_read10k_sleep10)

cache_workload.options.report_interval=10
# max operational latency in milli seconds.
cache_workload.options.max_latency=1000
cache_workload.options.run_time=200
cache_workload.options.sample_rate=1
cache_workload.options.warmup=0
cache_workload.options.sample_interval_ms = 1000

ret = cache_workload.run(conn)
assert ret == 0, ret

# print stats after workload run
cache_eviction_file = context.args.home + "/cache_eviction.stat"
get_cache_eviction_stats(s, cache_eviction_file)

latency_filename = context.args.home + "/latency.stat"
latency.workload_latency(cache_workload, latency_filename)
