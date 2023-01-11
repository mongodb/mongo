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

# This workload tests the continuous creation and deletion of tables.

from runner import *
from wiredtiger import *
from workgen import *

context = Context()
connection = context.wiredtiger_open("create")
session = connection.open_session()

key = Key(Key.KEYGEN_APPEND, 10)
value = Value(40)

# Create operations that work on random tables.
op_ins_rnd = Operation(Operation.OP_INSERT, key, value)
op_upd_rnd = Operation(Operation.OP_UPDATE, key, value)
op_read_rnd = Operation(Operation.OP_SEARCH, key, value)
thread_ins_rnd = Thread(op_ins_rnd * 10)
thread_upd_rnd = Thread(op_upd_rnd * 5)
thread_read_rnd = Thread(op_read_rnd * 5)

workload = Workload(context, thread_ins_rnd + thread_upd_rnd + thread_read_rnd)
workload.options.run_time = 300

# Define a workload thread to create tables periodically:
#   - Start creating tables when the database size falls below 100 MB.
#   - Create 3 tables every 5 seconds until the database size reaches 200 MB.
#   - Stop creating tables when the database size exceeds 200 MB.
#   - Add a prefix to the names of all the tables created using this thread.
workload.options.create_prefix = "dynamic_"
workload.options.create_interval = 5
workload.options.create_count = 3
workload.options.create_trigger = 100
workload.options.create_target = 200

# Define a workload thread to drop tables periodically:
#   - Start dropping tables when the database size exceeds 250 MB.
#   - Randomly drop 2 tables every 3 seconds until the database size reaches 75 MB.
#   - Stop dropping tables when the database size goes below 75 MB.
workload.options.drop_interval = 3
workload.options.drop_count = 2
workload.options.drop_trigger = 250
workload.options.drop_target = 75

ret = workload.run(connection)
assert ret == 0, ret
connection.close()
