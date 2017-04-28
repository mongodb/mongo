#!/usr/bin/python
# Drive a constant high workload through, even if WiredTiger isn't keeping
# up by dividing the workload across a lot of threads. This needs to be
# tuned to the particular machine so the workload is close to capacity in the
# steady state, but not overwhelming.
#
################
# Note: as a proof of concept for workgen, this matches closely
# bench/wtperf/runner/multi-btree-read-heavy-stress.wtperf .
# Run time, #ops, #threads are ratcheted way down for testing.
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
## cache_size=20GB
conn_config="create,cache_size=1GB,session_max=1000,eviction=(threads_min=4,threads_max=8),log=(enabled=false),transaction_sync=(enabled=false),checkpoint_sync=true,checkpoint=(wait=60),statistics=(fast),statistics_log=(json,wait=1)"
table_config="allocation_size=4k,memory_page_max=10MB,prefix_compression=false,split_pct=90,leaf_page_max=32k,internal_page_max=16k,type=file,block_compressor=snappy"
conn_config += extensions_config(['compressors/snappy'])
conn = wiredtiger_open("WT_TEST", conn_config)
s = conn.open_session()

tables = []
for i in range(0, 8):
    tname = "table:test" + str(i)
    s.create(tname, 'key_format=S,value_format=S,' + table_config)
    tables.append(Table(tname))
tname = "table:log"
# TODO: use table_config for the log file?
s.create(tname, 'key_format=S,value_format=S,' + table_config)
logtable = Table(tname)

##icount=200000000 / 8
icount=20000
ins_ops = operations(Operation.OP_INSERT, tables, Key(Key.KEYGEN_APPEND, 20), Value(500))
thread = Thread(ins_ops * icount)
pop_workload = Workload(context, thread)
print('populate:')
pop_workload.run(conn)

ins_ops = operations(Operation.OP_INSERT, tables, Key(Key.KEYGEN_APPEND, 20), Value(500), 0, logtable)
upd_ops = operations(Operation.OP_UPDATE, tables, Key(Key.KEYGEN_UNIFORM, 20), Value(500), 0, logtable)
read_ops = operations(Operation.OP_SEARCH, tables, Key(Key.KEYGEN_UNIFORM, 20), None, 3)

ins_thread = Thread(ins_ops)
upd_thread = Thread(upd_ops)
read_thread = Thread(read_ops)
ins_thread.options.throttle = 250
ins_thread.options.name = "Insert"
upd_thread.options.throttle = 250
upd_thread.options.name = "Update"
read_thread.options.throttle = 1000
read_thread.options.name = "Read"
##threads = [ins_thread] * 10 + [upd_thread] * 10 + [read_thread] * 80
threads = ins_thread * 1 + upd_thread * 1 + read_thread * 2
workload = Workload(context, threads)
##workload.options.run_time = 3600
workload.options.run_time = 30
workload.options.report_interval = 1
workload.options.sample_interval = 5
workload.options.sample_rate = 1
print('heavy stress workload:')
workload.run(conn)

latency_filename = conn.get_home() + '/latency.out'
print('for latency output, see: ' + latency_filename)
latency.workload_latency(workload, latency_filename)
