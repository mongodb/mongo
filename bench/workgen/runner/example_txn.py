#!/usr/bin/python
from runner import *
from wiredtiger import *
from workgen import *

conn = wiredtiger_open("WT_TEST", "create,cache_size=500MB")
s = conn.open_session()
tname = "table:test"
s.create(tname, 'key_format=S,value_format=S')
table = Table(tname)
table.options.key_size = 20
table.options.value_size = 100

context = Context()
op = Operation(Operation.OP_INSERT, table)
thread = Thread(op * 500000)
pop_workload = Workload(context, thread)
print('populate:')
pop_workload.run(conn)

opread = Operation(Operation.OP_SEARCH, table)
opwrite = Operation(Operation.OP_INSERT, table)
treader = Thread(opread)
twriter = Thread(txn(opwrite * 2))
workload = Workload(context, treader * 8 + twriter * 2)
workload.options.run_time = 10
workload.options.report_interval = 5
print('transactional write workload:')
workload.run(conn)
