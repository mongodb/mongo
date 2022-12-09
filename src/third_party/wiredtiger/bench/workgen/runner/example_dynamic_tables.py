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

# This workload simulates the continuous creation of tables.

import threading as pythread
import random
import string
import time

from runner import *
from wiredtiger import *
from workgen import *

class ThreadWithReturnValue(pythread.Thread):
    def __init__(self, group=None, target=None, name=None, args=(), kwargs={}):
        pythread.Thread.__init__(self, group, target, name, args, kwargs)
        self._return = None

    def run(self):
        self._return = self._target(*self._args, **self._kwargs)

    def join(self, *args):
        pythread.Thread.join(self, *args)
        return self._return

def generate_random_string(length):
    assert length > 0
    characters = string.ascii_letters + string.digits
    str = ''.join(random.choice(characters) for _ in range(length))
    return str

def create(session, workload, table_config):
    global tables

    # Generate a random name.
    name_length = 10
    table_name = "table:" + generate_random_string(name_length)
    try:
        session.create(table_name, table_config)
        # This indicates Workgen a new table exists.
        workload.create_table(table_name)
        tables.append(table_name)
    # Collision may occur.
    except RuntimeError as e:
        assert "already exists" in str(e).lower()

context = Context()
connection = context.wiredtiger_open("create")
session = connection.open_session()

# List of all tables.
tables = []

# Create a table.
table_config = 'key_format=S,value_format=S'
table_name = 'table:simple'
session.create(table_name, table_config)
tables.append(table_name)

key = Key(Key.KEYGEN_APPEND, 10)
value = Value(40)

# Create an operation dedicated to one table.
op = Operation(Operation.OP_INSERT, Table(table_name), key, value)
thread = Thread(op)

# Create operations that work on random tables.
op_ins_rnd = Operation(Operation.OP_INSERT, key, value)
op_upd_rnd = Operation(Operation.OP_UPDATE, key, value)
op_read_rnd = Operation(Operation.OP_SEARCH, key, value)
thread_ins_rnd = Thread(op_ins_rnd * 10)
thread_upd_rnd = Thread(op_upd_rnd * 10)
thread_read_rnd = Thread(op_read_rnd * 10)

workload = Workload(context, thread + thread_ins_rnd + thread_upd_rnd + thread_read_rnd)
workload.options.run_time = 10

# Start the workload.
workload_thread = ThreadWithReturnValue(target=workload.run, args=([connection]))
workload_thread.start()

# Create tables while the workload is running.
while workload_thread.is_alive():
    create(session, workload, table_config)
    time.sleep(1)

assert workload_thread.join() == 0

# Check tables match between Python and Workgen.
workgen_tables = workload.get_tables()

assert len(tables) == len(workgen_tables)
for t in tables:
    assert t in workgen_tables
