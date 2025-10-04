#!/usr/bin/env python3
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
# This workload is used to measure the impact of running checkpoint with flush_tier calls.
# The latency output files generated from this test will be compared against latency files
# of other workload.
#

from runner import *
from workgen import *
from microbenchmark_rts_unstable_content import timestamp_str, show

def large_updates(session, uri, value, start, end, timestamp):
    cursor = session.open_cursor(uri)
    for i in range(start, end + 1):
        session.begin_transaction()
        cursor[str(i)] = value
        session.commit_transaction('commit_timestamp=' + timestamp_str(timestamp))
    cursor.close()

nrows = 10000000
unstable = nrows // 10
uri = "table:rts_large_hs"
value = "aaaa"

context = Context()
conn = context.wiredtiger_open("create")
session = conn.open_session()
session.create(uri, "key_format=S,value_format=S")

# Write a large amount of data out with a stable timestamp.
large_updates(session, uri, value, nrows - unstable, nrows, 5)

# Write a small amount of data out with an unstable timestamp.
large_updates(session, uri, value, 0, unstable, 10)
session.checkpoint()
session.close()

session2 = conn.open_session()
# Write out data to history store.
for i in range(0, nrows + 1):
    session2.begin_transaction()
    cursor = session2.open_cursor(uri)
    cursor[str(i)] = "bbbb"
    session2.commit_transaction('commit_timestamp=' + timestamp_str(15))
cursor.close()
session2.checkpoint()

conn.set_timestamp('stable_timestamp=5')
conn.rollback_to_stable()

ops = Operation(Operation.OP_RTS, "")
thread = Thread(ops)
workload = Workload(context, thread)
workload.options.report_interval = 5
workload.options.max_latency = 10
workload.options.sample_rate = 1
workload.options.sample_interval_ms = 10

ret = workload.run(conn)
assert ret == 0, ret
latency.workload_latency(workload, 'rts_large_hs.out')
show(uri, session, context.args)
