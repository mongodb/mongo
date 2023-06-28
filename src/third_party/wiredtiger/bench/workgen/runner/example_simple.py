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

def show(tname, s, args):
    if not args.verbose:
        return
    print('')
    print('<><><><> ' + tname + ' <><><><>')
    c = s.open_cursor(tname, None)
    for k,v in c:
        print('key: ' + k)
        print('value: ' + v)
    print('<><><><><><><><><><><><>')
    c.close()

context = Context()

# Using the context's wiredtiger_open() method has benefits:
#   * there is a default home directory (WT_TEST), which is automatically cleared before the open.
#   * the args on the python command line are parsed, allowing for:
#     --home homedir
#     --keep   (don't remove homedir before starting)
#     --verbose
#     and the ability to add additional command line arguments.

conn = context.wiredtiger_open("create,cache_size=1G")
s = conn.open_session()
tname = 'table:simple'
s.create(tname, 'key_format=S,value_format=S')

ops = Operation(Operation.OP_INSERT, Table(tname), Key(Key.KEYGEN_APPEND, 10), Value(40))
thread = Thread(ops)
workload = Workload(context, thread)
ret = workload.run(conn)
assert ret == 0, ret
show(tname, s, context.args)

thread = Thread(ops * 5)
workload = Workload(context, thread)
ret = workload.run(conn)
assert ret == 0, ret
show(tname, s, context.args)
