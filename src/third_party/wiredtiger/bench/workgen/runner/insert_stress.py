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
conn_config="create,cache_size=4GB,session_max=1000,eviction=(threads_min=4,threads_max=8),log=(enabled=false),transaction_sync=(enabled=false),checkpoint_sync=true,checkpoint=(wait=10),statistics=(fast),statistics_log=(json,wait=1)"
table_config="allocation_size=4k,memory_page_max=10MB,prefix_compression=false,split_pct=90,leaf_page_max=32k,internal_page_max=16k,type=file,block_compressor=snappy"
conn = context.wiredtiger_open(conn_config)
s = conn.open_session()
tname = "file:test.wt"
table_config="key_format=S,value_format=S,allocation_size=4k,memory_page_max=10MB,prefix_compression=false,split_pct=90,leaf_page_max=32k,leaf_value_max=64MB,internal_page_max=16k,type=file,block_compressor=snappy"
s.create(tname, table_config)
table = Table(tname)
table.options.key_size = 20
table.options.value_size = 130 * 1024
table.options.range = 100000000 # 100 million

op = Operation(Operation.OP_INSERT, table)
thread = Thread(op * 500)
pop_workload = Workload(context, thread)
print('populate:')
ret = pop_workload.run(conn)
assert ret == 0, ret

op = Operation(Operation.OP_INSERT, table, Key(Key.KEYGEN_UNIFORM, 10), Value(130 * 1024))
op2 = Operation(Operation.OP_INSERT, table, Key(Key.KEYGEN_UNIFORM, 10), Value(100))
op3 = Operation(Operation.OP_INSERT, table, Key(Key.KEYGEN_APPEND, 10), Value(130 * 1024))
t = Thread(op + 10 * op2 + op3)

read_op = Operation(Operation.OP_SEARCH, table, Key(Key.KEYGEN_UNIFORM, 10))
read_txn_ops = op_group_transaction(read_op, 100, "")
read_thread = Thread(read_txn_ops)

workload = Workload(context, t * 8 + read_thread)
workload.options.run_time = 240
workload.options.report_interval = 5
print('workload:')
ret = workload.run(conn)
assert ret == 0, ret
