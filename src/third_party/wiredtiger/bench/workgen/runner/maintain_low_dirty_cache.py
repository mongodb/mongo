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

# Drive a constant high workload through, even if WiredTiger isn't keeping
# up by dividing the workload across a lot of threads. This needs to be
# tuned to the particular machine so the workload is close to capacity in the
# steady state, but not overwhelming.
#
################
# Note: This looks similar to multi_btree_heavy_stress.py with values altered
# for run time, #ops, #threads, #throttle to maintain dirty cache around the
# eviction target of 5% on the AWS perf machines. These values being machine
# dependant might need to be altered as per the machine this workload gets
# run on.
#
from runner import *
from wiredtiger import *
from workgen import *

def op_append(ops, op):
    if ops == None:
        ops = op
    else:
        ops += op
    return ops

def make_op(optype, table, key, value = None):
    if value == None:
        return Operation(optype, table, key)
    else:
        return Operation(optype, table, key, value)

logkey = Key(Key.KEYGEN_APPEND, 8)  ## should be 8 bytes format 'Q'
def operations(optype, tables, key, value = None, ops_per_txn = 0, logtable = None):
    txn_list = []
    ops = None
    nops = 0
    for table in tables:
        ops = op_append(ops, make_op(optype, table, key, value))
        if logtable != None:
            ops = op_append(ops, make_op(optype, logtable, logkey, value))
        nops += 1
        if ops_per_txn > 0 and nops % ops_per_txn == 0:
            txn_list.append(txn(ops))
            ops = None
    if ops_per_txn > 0:
        if ops != None:
            txn_list.append(txn(ops))
            ops = None
        for t in txn_list:
            ops = op_append(ops, t)
    return ops

context = Context()
conn_config="create,cache_size=2GB,session_max=1000,eviction=(threads_min=4,threads_max=8),log=(enabled=false),transaction_sync=(enabled=false),checkpoint_sync=true,checkpoint=(wait=8),statistics=(fast),statistics_log=(json,wait=1)"
table_config="allocation_size=4k,memory_page_max=10MB,prefix_compression=false,split_pct=90,leaf_page_max=32k,internal_page_max=16k,type=file,block_compressor=snappy"
conn = context.wiredtiger_open(conn_config)
s = conn.open_session()

tables = []
for i in range(0, 8):
    tname = "table:test" + str(i)
    s.create(tname, 'key_format=S,value_format=S,' + table_config)
    tables.append(Table(tname))
tname = "table:log"
s.create(tname, 'key_format=S,value_format=S,' + table_config)
logtable = Table(tname)

icount=1000000
ins_ops = operations(Operation.OP_INSERT, tables, Key(Key.KEYGEN_APPEND, 20), Value(500))
thread = Thread(ins_ops * icount)
pop_workload = Workload(context, thread)
print('populate:')
ret = pop_workload.run(conn)
assert ret == 0, ret

ins_ops = operations(Operation.OP_INSERT, tables, Key(Key.KEYGEN_APPEND, 20), Value(500), 0, logtable)
upd_ops = operations(Operation.OP_UPDATE, tables, Key(Key.KEYGEN_UNIFORM, 20), Value(500), 0, logtable)
read_ops = operations(Operation.OP_SEARCH, tables, Key(Key.KEYGEN_UNIFORM, 20), None, 3)

ins_thread = Thread(ins_ops)
upd_thread = Thread(upd_ops)
read_thread = Thread(read_ops)
ins_thread.options.throttle = 500
ins_thread.options.name = "Insert"
upd_thread.options.throttle = 500
upd_thread.options.name = "Update"
read_thread.options.throttle = 0
read_thread.options.name = "Read"
threads = ins_thread * 4 + upd_thread * 9 + read_thread * 90
workload = Workload(context, threads)
workload.options.run_time = 1200
workload.options.report_interval = 1
workload.options.sample_interval_ms = 5000
workload.options.sample_rate = 1
print('heavy stress workload:')
ret = workload.run(conn)
assert ret == 0, ret

latency_filename = conn.get_home() + '/latency.out'
print('for latency output, see: ' + latency_filename)
latency.workload_latency(workload, latency_filename)
