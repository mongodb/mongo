#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

import sys, threading, wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wiredtiger import WiredTigerError
from wtscenario import check_scenarios

class Callback(wiredtiger.AsyncCallback):
    def __init__(self, current):
        self.current = current
        self.ncompact = 0
        self.ninsert = 0
        self.nremove = 0
        self.nsearch = 0
        self.nupdate = 0
        self.nerror = 0
        self.lock = threading.RLock()

    def notify_error(self, key, value, optype, exp, desc):
        self.tty('ERROR: notify(' + str(key) + ',' + str(value) + ',' +
            str(optype) + '): ' + 'Expected: ' + str(exp) + ' ' + desc)

    def notify(self, op, op_ret, flags):

        # Note: we are careful not to throw any errors here.  Any
        # exceptions would be swallowed by a non-python worker thread.
        try:
            optype = op.get_type()
            if optype != wiredtiger.WT_AOP_COMPACT:
                key = op.get_key()
                #
                # Remove does not set a value.  Just set it from the
                # reference list.  If an error is expected, the
                # reference list should list it as the expected value.
                #
                if op_ret != 0:
                    value = op_ret
                elif optype != wiredtiger.WT_AOP_REMOVE:
                    value = op.get_value()
                else:
                    value = self.current[key]

            if optype == wiredtiger.WT_AOP_INSERT:
                self.lock.acquire()
                self.ninsert += 1
                self.lock.release()
            elif optype == wiredtiger.WT_AOP_COMPACT:
                self.lock.acquire()
                self.ncompact += 1
                self.lock.release()
                # Skip checking key/value.
                return 0
            elif optype == wiredtiger.WT_AOP_REMOVE:
                self.lock.acquire()
                self.nremove += 1
                self.lock.release()
            elif optype == wiredtiger.WT_AOP_SEARCH:
                self.lock.acquire()
                self.nsearch += 1
                self.lock.release()
            elif optype == wiredtiger.WT_AOP_UPDATE:
                self.lock.acquire()
                self.nupdate += 1
                self.lock.release()
            else:
                self.notify_error(key, value, optype, 0, 'unexpected optype')
                self.lock.acquire()
                self.nerror += 1
                self.lock.release()
            if self.current[key] != value:
                self.notify_error(key, value, optype, self.current[key], 'unexpected value')
                self.lock.acquire()
                self.nerror += 1
                self.lock.release()
        except (BaseException) as err:
            self.tty('ERROR: exception in notify: ' + str(err))
            raise

        return 0


# test_async02.py
#    Async operations
# Basic smoke-test of file and table async ops: tests get/set key, insert
# update, and remove.
class test_async02(wttest.WiredTigerTestCase, suite_subprocess):
    """
    Test basic operations
    """
    table_name1 = 'test_async02'
    nentries = 100
    async_ops = nentries / 2
    async_threads = 3
    current = {}

    scenarios = check_scenarios([
        ('file-col', dict(tablekind='col',uri='file')),
        ('file-fix', dict(tablekind='fix',uri='file')),
        ('file-row', dict(tablekind='row',uri='file')),
        ('lsm-row', dict(tablekind='row',uri='lsm')),
        ('table-col', dict(tablekind='col',uri='table')),
        ('table-fix', dict(tablekind='fix',uri='table')),
        ('table-row', dict(tablekind='row',uri='table')),
    ])

    # Enable async for this test.
    def conn_config(self, dir):
        return 'async=(enabled=true,ops_max=%s,' % self.async_ops + \
            'threads=%s)' % self.async_threads

    def genkey(self, i):
        if self.tablekind == 'row':
            return 'key' + str(i)
        else:
            return long(i+1)

    def genvalue(self, i):
        if self.tablekind == 'fix':
            return int(i & 0xff)
        else:
            return 'value' + str(i)

    # Create and populate the object.
    def create_session(self, tablearg):
        if self.tablekind == 'row':
            keyformat = 'key_format=S'
        else:
            keyformat = 'key_format=r'  # record format
        if self.tablekind == 'fix':
            valformat = 'value_format=8t'
        else:
            valformat = 'value_format=S'
        create_args = keyformat + ',' + valformat

        self.pr('creating session: ' + create_args)
        self.session.create(tablearg, create_args)

    def test_ops(self):
        tablearg = self.uri + ':' + self.table_name1
        self.create_session(tablearg)

        # Populate our reference table first, so we don't need to
        # use locks to reference it.
        self.current = {}
        for i in range(0, self.nentries):
            k = self.genkey(i)
            v = self.genvalue(i)
            self.current[k] = v

        # Populate table with async inserts, callback checks
        # to ensure key/value is correct.
        callback = Callback(self.current)
        for i in range(0, self.nentries):
            self.pr('creating async op')
            op = self.conn.async_new_op(tablearg, None, callback)
            k = self.genkey(i)
            v = self.genvalue(i)
            op.set_key(k)
            op.set_value(v)
            op.insert()

        self.conn.async_flush()
        self.pr('flushed')

        k = self.genkey(self.nentries / 2)
        v = self.genvalue(self.nentries / 2)
        k1 = self.genkey(self.nentries + 1)
        v1 = self.genvalue(self.nentries + 1)
        self.current[k] = wiredtiger.WT_DUPLICATE_KEY
        self.current[k1] = wiredtiger.WT_NOTFOUND
        #
        # Error cases:
        # Check inserting an existing record - WT_DUPLICATE_KEY.
        # Check updating a non-existent record - WT_NOTFOUND.
        # Check removing a non-existent record - WT_NOTFOUND.
        #
        op = self.conn.async_new_op(tablearg, 'overwrite=false', callback)
        op.set_key(k)
        op.set_value(v)
        op.insert()
        op = self.conn.async_new_op(tablearg, 'overwrite=false', callback)
        op.set_key(k1)
        op.set_value(v1)
        op.update()
        op = self.conn.async_new_op(tablearg, 'overwrite=false', callback)
        op.set_key(k1)
        op.remove()

        # Wait for all outstanding async ops to finish.
        self.conn.async_flush()
        self.pr('flushed')

        # Make sure all callbacks went according to plan.
        self.assertTrue(callback.nerror == 0)


if __name__ == '__main__':
    wttest.run()
