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
# Read heavy workload: ~90% reads, ~5% inserts, ~5% updates + checkpoints,
# readers use read_timestamp close to stable_timestamp.
#

from runner import *
from wiredtiger import *
from workgen import *
import os
import time

context = Context()
wt_builddir = os.getenv('WT_BUILDDIR')

# Connection: disagg + 8GB cache, extended cache_stuck_timeout_ms
conn_config = (
    f"cache_size=8GB,precise_checkpoint=true,"
    f"disaggregated=(drain_threads=2,page_log=palite,role=leader),"
    f"extensions=(\"{wt_builddir}/ext/page_log/palite/libwiredtiger_palite.so\"=),"
    f"cache_stuck_timeout_ms=300000"
)
conn = context.wiredtiger_open("create," + conn_config)
s = conn.open_session("")
conn.set_timestamp("stable_timestamp=1")

# Table: layered/disagg with small pages
table_config = (
    "key_format=S,value_format=S,"
    "exclusive=true,allocation_size=4kb,"
    "internal_page_max=4kb,leaf_page_max=4kb,split_pct=100,"
    "type=layered,block_manager=disagg"
)

tname = "table:test"
table = Table(tname)
s.create(tname, table_config)
table.options.key_size = 20
table.options.value_size = 500

# ------------------------
# Populate: 500K rows
# ------------------------
populate_threads = 8
icount = 500_000

pop_ops = Operation(Operation.OP_INSERT, table)
nops_per_thread = icount // populate_threads
pop_thread = Thread(pop_ops * nops_per_thread)
pop_workload = Workload(context, populate_threads * pop_thread)

print("populate Start:")
start_time = time.time()
ret = pop_workload.run(conn)
print("populate End:")
end_time = time.time()
print("Populate took %d minutes" % ((end_time - start_time) // 60))
assert ret == 0, ret

# ------------------------
# Run phase: writers + ckpt
# ------------------------

# Updates in snapshot transactions (~5% of total ops)
op_update = Operation(Operation.OP_UPDATE, table)
# If you want skewed updates, use Pareto instead:
# op_update = Operation(Operation.OP_UPDATE, table,
#                       Key(Key.KEYGEN_PARETO, 0, ParetoOptions(10)))
op_update = txn(op_update, 'isolation=snapshot')
op_update.transaction.use_commit_timestamp = True
tupdate = Thread(op_update * 1000)  # 1000 update ops per update thread
tupdate.options.session_config = "isolation=snapshot"

# Inserts in snapshot transactions (~5% of total ops)
op_insert = Operation(Operation.OP_INSERT, table)
op_insert = txn(op_insert, 'isolation=snapshot')
op_insert.transaction.use_commit_timestamp = True
tinsert = Thread(op_insert * 1000)  # 1000 insert ops per insert thread
tinsert.options.session_config = "isolation=snapshot"

# ------------------------
# Lagged readers near stable_timestamp (~90% of total ops)
# ------------------------
# stable_timestamp_lag = 50 below; these lags are just behind it.
lags = [60, 75, 90, 105, 120, 135, 150, 165, 180, 195]
reader_threads = []

# Reads: 10 lags * 9 threads/lag * 1000 ops => 90,000 read ops total
for lag in lags:
    op_read = Operation(Operation.OP_SEARCH, table)
    read_txn = txn(op_read, 'read_timestamp')
    read_txn.transaction.read_timestamp_lag = lag
    tread = Thread(read_txn * 1000)
    tread.options.session_config = "isolation=snapshot"
    reader_threads.append(tread)

# ------------------------
# Checkpoint every 10 seconds (more frequent)
# ------------------------
checkpoint_ops = Operation(Operation.OP_SLEEP, "10") + \
    Operation(Operation.OP_CHECKPOINT, "")
# 900s / 10s = 90 checkpoints over the run
ops = checkpoint_ops * 90
checkpoint_thread = Thread(ops)

# ------------------------
# Assemble workload threads
# ------------------------
# Writes: 5 * tupdate (5k updates) + 5 * tinsert (5k inserts) => 10k write ops
threads = 5 * tupdate + 5 * tinsert

# Reads: 10 lags * 9 threads each * 1000 ops => 90k read ops
for tread in reader_threads:
    threads = threads + 9 * tread

threads = threads + checkpoint_thread

workload = Workload(context, threads)

workload.options.run_time = 900  # 15 minutes
workload.options.report_interval = 10  # 10-second stats

# Timestamp lags
workload.options.oldest_timestamp_lag = 350
workload.options.stable_timestamp_lag = 50
workload.options.timestamp_advance = 1

print("disagg workload: read-heavy with checkpoints:")
start_time = time.time()
ret = workload.run(conn)
print("workload.run returned:", ret)
end_time = time.time()
print("Workload took %d minutes" % ((end_time - start_time) // 60))

latency_filename = os.path.join(context.args.home, "latency.out")
latency.workload_latency(workload, latency_filename)

conn.close()

