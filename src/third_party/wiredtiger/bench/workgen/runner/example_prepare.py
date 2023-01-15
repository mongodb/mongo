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
from wiredtiger import *
from workgen import *
import time

context = Context()
conn = context.wiredtiger_open("create,cache_size=500MB")
s = conn.open_session()
tname = "table:test"
config = "key_format=S,value_format=S,"
s.create(tname, config)
table = Table(tname)
table.options.key_size = 20
table.options.value_size = 10

start_time = time.time()

op = Operation(Operation.OP_INSERT, table)
thread = Thread(op * 5000)
pop_workload = Workload(context, thread)
print('populate:')
ret = pop_workload.run(conn)
assert ret == 0, ret

opread = Operation(Operation.OP_SEARCH, table)
read_txn = txn(opread, 'read_timestamp')
# read_timestamp_lag is the lag to the read_timestamp from current time
read_txn.transaction.read_timestamp_lag = 2
treader = Thread(read_txn)

opwrite = Operation(Operation.OP_INSERT, table)
write_txn = txn(opwrite, 'isolation=snapshot')
# use_prepare_timestamp - Commit the transaction with stable_timestamp.
write_txn.transaction.use_prepare_timestamp = True
twriter = Thread(write_txn)
# Thread.options.session_config - Session configuration.
twriter.options.session_config="isolation=snapshot"

opupdate = Operation(Operation.OP_UPDATE, table)
update_txn = txn(opupdate, 'isolation=snapshot')
# use_commit_timestamp - Commit the transaction with commit_timestamp.
update_txn.transaction.use_commit_timestamp = True
tupdate = Thread(update_txn)
# Thread.options.session_config - Session configuration.
tupdate.options.session_config="isolation=snapshot"

workload = Workload(context, 30 * twriter + 30 * tupdate + 30 * treader)
workload.options.run_time = 50
workload.options.report_interval=500
# read_timestamp_lag - Number of seconds lag to the oldest_timestamp from current time.
workload.options.oldest_timestamp_lag=30
# read_timestamp_lag - Number of seconds lag to the stable_timestamp from current time.
workload.options.stable_timestamp_lag=10
# timestamp_advance is the number of seconds to wait before moving oldest and stable timestamp.
workload.options.timestamp_advance=1
print('transactional prepare workload:')
ret = workload.run(conn)
assert ret == 0, ret

end_time = time.time()
run_time = end_time - start_time

print('Workload took %d minutes' %(run_time//60))

latency_filename = os.path.join(context.args.home, "latency.out")
latency.workload_latency(workload, latency_filename)
conn.close()
