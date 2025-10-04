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

nrows = 1
ntables = 100000
uri = "table:rts_many_files"
context = Context()
conn = context.wiredtiger_open("create")
session = conn.open_session()

for i in range(0, ntables):
    session.create(uri + str(i), "key_format=S,value_format=S")

# Write out some data.
for i in range(0, ntables):
    cursor = session.open_cursor(uri + str(i))
    session.begin_transaction()
    for j in range(1, nrows + 1):
        cursor[str(j)] = "aaaa" + str(j)
        # Don't let transactions get too big.
        if i % 56 == 0:
            session.commit_transaction()
            session.begin_transaction()
    session.commit_transaction()
    cursor.close()

# Evict our data out of paranoia.
for i in range(0, ntables):
    evict_cursor = session.open_cursor(uri + str(i), None, "debug=(release_evict)")
    session.begin_transaction()
    for j in range(1, nrows + 1):
        evict_cursor.set_key(str(j))
        res = evict_cursor.search()
        if res != 0:
            raise Exception("Uh Oh! Something went wrong evicting data!")
        evict_cursor.reset()
    session.rollback_transaction()
    evict_cursor.close()

ops = Operation(Operation.OP_CHECKPOINT, "") + Operation(Operation.OP_RTS, "")
thread = Thread(ops)
workload = Workload(context, thread)
workload.options.report_interval = 5
workload.options.max_latency = 10
workload.options.sample_rate = 1
workload.options.sample_interval_ms = 10

ret = workload.run(conn)
assert ret == 0, ret

latency.workload_latency(workload, 'rts_many_files.out')
show(uri, session, context.args)
