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

# This benchmark is designed to stress disk access to the history store file.
# This is achieved through:
#   - Long running transactions consisting of read and update operations.
#   - Low cache size (~20%) for a reasonably sized WT table with large documents.
#   - Pareto distribution for operations in long running transactions. This will cause
#     skewed access for a selective set of keys in WT table.
#   - Relatively large number of read operation threads to stress cache.

# Benchmark is based on a wtperf config: wiredtiger/bench/wtperf/runners/evict-btree-scan.wtperf
# There are number of changes made to original configs such as:
#   - Reduced the number of documents inserts during initial warm-up phase.
#   - Increased the sizes of key and value.
#   - Reduced the WT cache size.
#   - Added transaction based operations and repurposed some of the read threads as long-running
#     transcation threads.

###################################################################################################
'''
# wtperf options file: evict btree configuration
conn_config="cache_size=40G,checkpoint=(wait=60,log_size=2GB),eviction=(threads_min=12,
threads_max=12),log=(enabled=true),session_max=600,eviction_target=60,statistics=(fast),
statistics_log=(wait=1,json)"

# 1B records * (key=12 + value=138) is about 150G total data size
key_sz=12
value_sz=138
log_like_table=true
table_config="type=file"
icount=1000000000
report_interval=5
run_time=3600
# Scans every 10 minutes for all the scan specific tables.
# .4B records * (key=12 + value=138) is about 60G total data size for scan
# Running on a machine with 64G physical memory, this exhausts both the
# WT cache and the system cache.
scan_interval=600
scan_pct=100
scan_table_count=20
scan_icount=400000000
populate_threads=5
table_count=100
threads=((count=400,reads=1),(count=20,inserts=1,throttle=500),(count=10,updates=1,throttle=500))
# Add throughput/latency monitoring
max_latency=50000
sample_interval=5
'''
###################################################################################################

from runner import *
from wiredtiger import *
from workgen import *

context = Context()
conn_config =   "cache_size=1G,checkpoint=(wait=60,log_size=2GB),\
                eviction=(threads_min=12,threads_max=12),log=(enabled=true),session_max=800,\
                eviction_target=60,statistics=(fast),statistics_log=(wait=1,json)"# explicitly added
conn = context.wiredtiger_open("create," + conn_config)
s = conn.open_session("")

wtperf_table_config = "key_format=S,value_format=S," +\
    "exclusive=true,allocation_size=4kb," +\
    "internal_page_max=64kb,leaf_page_max=4kb,split_pct=100,"
compress_table_config = ""
table_config = "type=file"
tables = []
table_count = 1
for i in range(0, table_count):
    tname = "table:test" + str(i)
    table = Table(tname)
    s.create(tname, wtperf_table_config +\
             compress_table_config + table_config)
    table.options.key_size = 200
    table.options.value_size = 5000
    tables.append(table)

populate_threads = 40
icount = 500000
# If there are multiple tables to be filled during populate,
# the icount is split between them all.
pop_ops = Operation(Operation.OP_INSERT, tables[0])
pop_ops = op_multi_table(pop_ops, tables)
nops_per_thread = icount // (populate_threads * table_count)
pop_thread = Thread(pop_ops * nops_per_thread)
pop_workload = Workload(context, populate_threads * pop_thread)
ret = pop_workload.run(conn)
assert ret == 0, ret

# Log like file, requires that logging be enabled in the connection config.
log_name = "table:log"
s.create(log_name, wtperf_table_config + "key_format=S,value_format=S," +\
        compress_table_config + table_config + ",log=(enabled=true)")
log_table = Table(log_name)

ops = Operation(Operation.OP_SEARCH, tables[0],Key(Key.KEYGEN_PARETO, 0, ParetoOptions(1)))
ops = op_multi_table(ops, tables, False)
ops = op_log_like(ops, log_table, 0)
thread0 = Thread(ops)

ops = Operation(Operation.OP_INSERT, tables[0])
ops = op_multi_table(ops, tables, False)
ops = op_log_like(ops, log_table, 0)
thread1 = Thread(ops)
# These operations include log_like operations, which will increase the number
# of insert/update operations by a factor of 2.0. This may cause the
# actual operations performed to be above the throttle.

ops = Operation(Operation.OP_UPDATE, tables[0])
ops = op_multi_table(ops, tables, False)
ops = op_log_like(ops, log_table, 0)
thread2 = Thread(ops)
# These operations include log_like operations, which will increase the number
# of insert/update operations by a factor of 2.0. This may cause the
# actual operations performed to be above the throttle.
thread2.options.throttle=500
thread2.options.throttle_burst=1.0

# Long running transactions. There is a 0.1 second sleep after a series of search and update
# operations. The sleep op is repeated 10000 times and this will make these transcations to at
# least run for ~17 minutes.
search_op = Operation(Operation.OP_SEARCH, tables[0], Key(Key.KEYGEN_PARETO, 0, ParetoOptions(1)))
update_op = Operation(Operation.OP_UPDATE, tables[0], Key(Key.KEYGEN_PARETO, 0, ParetoOptions(1)))
ops = txn(((search_op + update_op) * 1000 + sleep(0.1)) * 10000)
thread3 = Thread(ops)

ops = Operation(Operation.OP_SLEEP, "0.1") + \
      Operation(Operation.OP_LOG_FLUSH, "")
logging_thread = Thread(ops)

workload = Workload(context, 400 * thread0 + 100 * thread1 +\
                    10 * thread2 + 100 * thread3 + logging_thread)
workload.options.report_interval=5
workload.options.run_time=500
workload.options.max_latency=50000
ret = workload.run(conn)
assert ret == 0, ret

latency_filename = os.path.join(context.args.home, "latency.out")
latency.workload_latency(workload, latency_filename)
conn.close()
