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
# test_readonly03.py
#   Readonly: Test connection readonly mode with modifying methods.  Confirm
#   all return ENOTSUP.
#

import wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet

class test_readonly03(wttest.WiredTigerTestCase, suite_subprocess):
    uri = 'table:test_readonly03'
    uri2 = 'table:test_readonly03_2'
    create = True

    conn_params = 'create,log=(enabled),operation_tracking=(enabled=false),'
    conn_params_rd = 'readonly=true,operation_tracking=(enabled=false),'

    session_ops = [ 'alter', 'create', 'compact', 'drop', 'flush_tier', 'log_flush',
        'log_printf', 'rename', 'salvage', 'truncate', 'upgrade', ]
    cursor_ops = [ 'insert', 'remove', 'update', ]

    def setUpConnectionOpen(self, dir):
        self.home = dir
        if self.create:
            conn_cfg = self.conn_params
        else:
            conn_cfg = self.conn_params_rd
        conn = self.wiredtiger_open(dir, conn_cfg)
        self.create = False
        return conn

    def test_readonly(self):
        create_params = 'key_format=i,value_format=i'
        entries = 10
        # Create a database and a table.
        SimpleDataSet(self, self.uri, entries, key_format='i',
            value_format='i').populate()

        #
        # Now close and reopen.  Note that the connection function
        # above will reopen it readonly.
        self.reopen_conn()
        msg = '/Unsupported/'
        c = self.session.open_cursor(self.uri, None, None)
        for op in self.cursor_ops:
            c.set_key(1)
            c.set_value(1)
            if op == 'insert':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: c.insert(), msg)
            elif op == 'remove':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: c.remove(), msg)
            elif op == 'update':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: c.update(), msg)
            else:
                self.fail('Unknown cursor operation: ' + op)
        c.close()
        for op in self.session_ops:
            if op == 'alter':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.alter(self.uri, None), msg)
            elif op == 'create':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.create(self.uri2, create_params),
                    msg)
            elif op == 'compact':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.compact(self.uri, None), msg)
            elif op == 'drop':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.drop(self.uri, None), msg)
            elif op == 'flush_tier':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.checkpoint('flush_tier=(enabled)'), msg)
            elif op == 'log_flush':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.log_flush(None), msg)
            elif op == 'log_printf':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.log_printf("test"), msg)
            elif op == 'rename':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.rename(self.uri, self.uri2, None), msg)
            elif op == 'salvage':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.salvage(self.uri, None), msg)
            elif op == 'truncate':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.truncate(self.uri, None, None, None),
                    msg)
            elif op == 'upgrade':
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                    lambda: self.session.upgrade(self.uri, None), msg)
            else:
                self.fail('Unknown session method: ' + op)

if __name__ == '__main__':
    wttest.run()
