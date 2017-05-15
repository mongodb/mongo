#!/usr/bin/python
from runner import *
from wiredtiger import *
from workgen import *

def tablename(id):
    return "table:test%06d" % id

def show(tname):
    print('')
    print('<><><><> ' + tname + ' <><><><>')
    c = s.open_cursor(tname, None)
    for k,v in c:
        print('key: ' + k)
        print('value: ' + v)
    print('<><><><><><><><><><><><>')
    c.close()

def expectException(expr):
    gotit = False
    try:
        expr()
    except BaseException as e:
        print('got expected exception: ' + str(e))
        gotit = True
    if not gotit:
        raise Exception("missing expected exception")

context = Context()
conn = wiredtiger_open("WT_TEST", "create,cache_size=1G")
s = conn.open_session()
tname0 = tablename(0)
tname1 = tablename(1)
s.create(tname0, 'key_format=S,value_format=S')
s.create(tname1, 'key_format=S,value_format=S')

ops = Operation(Operation.OP_INSERT, Table(tname0), Key(Key.KEYGEN_APPEND, 10), Value(100))
workload = Workload(context, Thread(ops))

print('RUN1')
workload.run(conn)
show(tname0)

# The context has memory of how many keys are in all the tables.
# truncate goes behind context's back, but it doesn't matter for
# an insert-only test.
s.truncate(tname0, None, None)

# Show how to 'multiply' operations
op = Operation(Operation.OP_INSERT, Table(tname0), Key(Key.KEYGEN_APPEND, 10), Value(100))
op2 = Operation(Operation.OP_INSERT, Table(tname1), Key(Key.KEYGEN_APPEND, 20), Value(30))
o = op2 * 10
print 'op is: ' + str(op)
print 'multiplying op is: ' + str(o)
thread0 = Thread(o + op + op)
workload = Workload(context, thread0)
print('RUN2')
workload.run(conn)
show(tname0)
show(tname1)

s.truncate(tname0, None, None)
s.truncate(tname1, None, None)

# operations can be multiplied, added in any combination.
op += Operation(Operation.OP_INSERT, Table(tname0), Key(Key.KEYGEN_APPEND, 10), Value(10))
op *= 2
op += Operation(Operation.OP_INSERT, Table(tname0), Key(Key.KEYGEN_APPEND, 10), Value(10))
thread0 = Thread(op * 10 + op2 * 20)
workload = Workload(context, thread0)
print('RUN3')
workload.run(conn)
show(tname0)
show(tname1)

print('workload is ' + str(workload))
print('thread0 is ' + str(thread0))

def assignit(k, n):
    k._size = n

expectException(lambda: Operation(
    Operation.OP_INSERT, Table('foo'), Key(Key.KEYGEN_APPEND, 10)))
# we don't catch this exception here, but in Workload.run()
k = Key(Key.KEYGEN_APPEND, 1)
assignit(k, 30)
assignit(k, 1)  # we don't catch this exception here, but in Workload.run()
op = Operation(Operation.OP_INSERT, Table(tname0), k, Value(10))
workload = Workload(context, Thread(op))
print('RUN4')
expectException(lambda: workload.run(conn))

print('HELP:')
print(workload.options.help())
