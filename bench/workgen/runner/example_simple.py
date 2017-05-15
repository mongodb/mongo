#!/usr/bin/python
from runner import *
from wiredtiger import *
from workgen import *

def show(tname):
    print('')
    print('<><><><> ' + tname + ' <><><><>')
    c = s.open_cursor(tname, None)
    for k,v in c:
        print('key: ' + k)
        print('value: ' + v)
    print('<><><><><><><><><><><><>')
    c.close()

context = Context()
conn = wiredtiger_open("WT_TEST", "create,cache_size=1G")
s = conn.open_session()
tname = 'table:simple'
s.create(tname, 'key_format=S,value_format=S')

ops = Operation(Operation.OP_INSERT, Table(tname), Key(Key.KEYGEN_APPEND, 10), Value(40))
thread = Thread(ops)
workload = Workload(context, thread)
workload.run(conn)
show(tname)

thread = Thread(ops * 5)
workload = Workload(context, thread)
workload.run(conn)
show(tname)
