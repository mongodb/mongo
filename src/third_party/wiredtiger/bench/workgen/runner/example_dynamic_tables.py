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

# This workload simulates the continuous creation and deletion of tables.

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
        workload.add_table(table_name)
        tables.append(table_name)
    # Collision may occur.
    except RuntimeError as e:
        assert "already exists" in str(e).lower()

def drop(workload, table_name):
    global tables

    try:
        # Mark the table for deletion in Workgen.
        workload.remove_table(table_name)
        # Delete local data.
        tables.remove(table_name)
    except RuntimeError as e:
        assert "it is part of the static set" in str(e).lower()

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

# Create and drop tables while the workload is running.
tables_to_delete = []
while workload_thread.is_alive():
    create(session, workload, table_config)

    # Select a random table to drop.
    if len(tables):
        idx = random.randint(0, len(tables) - 1)
        uri = tables[idx]
        # Mark it for deletion.
        drop(workload, uri)

    # Trigger the Workgen garbage collection.
    tables_to_delete += workload.garbage_collection()
    deleted_tables = []
    for t in tables_to_delete:
        try:
            session.drop(t)
            # If the WiredTiger call is successful, this table has been removed safely from all
            # layers.
            deleted_tables.append(t)
        except WiredTigerError as e:
            assert "device or resource busy" in str(e).lower()

    # Update the list of tables pending deletion.
    for t in deleted_tables:
        tables_to_delete.remove(t)

    time.sleep(1)

assert workload_thread.join() == 0

# It is possible that Python and Workgen are not matching at this point in terms of tables. Since
# Workgen cannot remove a table that is still being used, it may have kept reference to it.
# Now the workload is finished, call the garbage collector on Workgen, nothing should be blocking
# the deletion of tables that are supposed to be removed.
# Note that we are not checking what's on disk, we don't need to use what garbage_collection
# returns.
workload.garbage_collection()

# Check tables match between Python and Workgen.
workgen_tables = workload.get_tables()

assert len(tables) == len(workgen_tables)
for t in tables:
    assert t in workgen_tables
