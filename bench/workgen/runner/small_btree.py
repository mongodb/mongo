#!/usr/bin/python
from runner import *
from wiredtiger import *
from workgen import *

context = Context()
conn = wiredtiger_open("WT_TEST", "create,cache_size=500MB")
s = conn.open_session()
tname = "file:test.wt"
s.create(tname, 'key_format=S,value_format=S')
table = Table(tname)
table.options.key_size = 20
table.options.value_size = 100

op = Operation(Operation.OP_INSERT, table)
thread = Thread(op * 500000)
pop_workload = Workload(context, thread)
print('populate:')
pop_workload.run(conn)

op = Operation(Operation.OP_SEARCH, table)
t = Thread(op)
workload = Workload(context, t * 8)
workload.options.run_time = 120
workload.options.report_interval = 5
print('read workload:')
workload.run(conn)
