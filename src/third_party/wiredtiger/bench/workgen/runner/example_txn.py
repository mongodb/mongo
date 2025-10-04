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

context = Context()
conn = context.wiredtiger_open("create,cache_size=500MB")
s = conn.open_session()
tname = "table:test"
s.create(tname, 'key_format=S,value_format=S')
table = Table(tname)
table.options.key_size = 20
table.options.value_size = 100

op = Operation(Operation.OP_INSERT, table)
thread = Thread(op * 500000)
pop_workload = Workload(context, thread)
print('populate:')
ret = pop_workload.run(conn)
assert ret == 0, ret

opread = Operation(Operation.OP_SEARCH, table)
opwrite = Operation(Operation.OP_INSERT, table)
treader = Thread(opread)
twriter = Thread(txn(opwrite * 2))
workload = Workload(context, treader * 8 + twriter * 2)
workload.options.run_time = 10
workload.options.report_interval = 5
print('transactional write workload:')
ret = workload.run(conn)
assert ret == 0, ret
