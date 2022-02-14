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
# If unittest2 is available, use it in preference to (the old) unittest

import os
import shutil
import wiredtiger, wttest

# test_shared_cache02.py
#    Shared cache tests
# Test shared cache shared amongst multiple connections.
class test_shared_cache02(wttest.WiredTigerTestCase):

    uri = 'table:test_shared_cache02'
    # Setup fairly large items to use up cache
    data_str = 'abcdefghijklmnopqrstuvwxyz' * 20

    # Add a set of records
    def add_records(self, session, start, stop):
        cursor = session.open_cursor(self.uri, None, "overwrite")
        for i in range(start, stop+1):
            cursor["%010d KEY------" % i] = ("%010d VALUE " % i) + self.data_str
        cursor.close()

    # Disable default setup/shutdown steps - connections are managed manually.
    def setUpSessionOpen(self, conn):
        return None

    def close_conn(self):
        return None

    def setUpConnectionOpen(self, dir):
        return None

    def openConnections(
            self,
            connections,
            pool_opts = ',shared_cache=(name=pool,size=200M,chunk=10M,reserve=30M),',
            extra_opts = '',
            add=0):
        if add == 0:
            self.conns = []
            self.sessions = []
        # Open the set of connections.
        for name in connections:
            shutil.rmtree(name, True)
            os.mkdir(name)
            next_conn =  self.wiredtiger_open(
                name,
                'create,error_prefix="%s",' % self.shortid() +
                pool_opts + extra_opts)
            self.conns.append(next_conn)
            self.sessions.append(next_conn.open_session(None))
        return None

    def closeConnections(self):
        for tmp_conn in self.conns:
            tmp_conn.close()
        self.conns = []
        self.sessions = [] # Implicitly closed when closing sessions.

    # Test reconfigure API
    def test_shared_cache_reconfig01(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)

        connection = self.conns[0]
        connection.reconfigure("shared_cache=(name=pool,size=300M)")
        self.closeConnections()

    # Test reconfigure that grows the usage over quota fails
    def test_shared_cache_reconfig02(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'],
            pool_opts = ',shared_cache=(name=pool,size=50M,reserve=20M),')

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)

        connection = self.conns[0]
        # Reconfigure to over-subscribe, call should fail with an error
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: connection.reconfigure("shared_cache=(name=pool,reserve=40M)"),
            '/Shared cache unable to accommodate this configuration/')
        # TODO: Ensure that the reserve size wasn't updated.
        # cursor = self.sessions[0].open_cursor('config:', None, None)
        # value = cursor['connection']
        # self.assertTrue(value.find('reserve') != -1)

        self.closeConnections()

    # Test reconfigure that would grow the usage over quota if the
    # previous reserve size isn't taken into account
    def test_shared_cache_reconfig03(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'],
            pool_opts = ',shared_cache=(name=pool,size=50M,reserve=20M),')

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)

        connection = self.conns[0]

        connection.reconfigure("shared_cache=(name=pool,reserve=30M)"),

        # TODO: Ensure that the reserve size was updated.
        # cursor = self.sessions[0].open_cursor('config:', None, None)
        # value = cursor['connection']
        # self.assertTrue(value.find('reserve') != -1)

        self.closeConnections()

    # Test reconfigure that switches to using a shared cache
    # previous reserve size isn't taken into account
    def test_shared_cache_reconfig03(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'], pool_opts = ',')

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)

        self.conns[0].reconfigure("shared_cache=(name=pool,reserve=20M)"),
        self.conns[1].reconfigure("shared_cache=(name=pool,reserve=20M)"),

        # TODO: Ensure that the reserve size was updated.
        # cursor = self.sessions[0].open_cursor('config:', None, None)
        # value = cursor['connection']
        # self.assertTrue(value.find('reserve') != -1)

        self.closeConnections()

    # Test reconfigure with absolute value for eviction config fails
    def test_shared_cache_reconfig04(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'],
            pool_opts = ',shared_cache=(name=pool,size=50M,reserve=20M),')

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)

        connection = self.conns[0]
        # Reconfiguring with absolute value of eviction trigger should fail.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: connection.reconfigure("shared_cache=(name=pool,"
            "size=20M,reserve=10M),eviction_trigger=10M"),'/Shared cache '
            'configuration requires a percentage value for eviction trigger/')

        connection = self.conns[1]
        # Reconfiguring with absolute value for eviction target should fail.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: connection.reconfigure("shared_cache=(name=pool,"
            "size=20M,reserve=10M),eviction_target=10M"),'/Shared cache '
            'configuration requires a percentage value for eviction target/')

        # Reconfigure with percentage value for eviction target passes
        self.conns[0].reconfigure("shared_cache=(name=pool,reserve=20M),"
            "eviction_target=50")

        self.closeConnections()

if __name__ == '__main__':
    wttest.run()
