#!/usr/bin/env python
#
# Public Domain 2008-2012 WiredTiger, Inc.
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
try:
    import unittest2 as unittest
except ImportError:
    import unittest

import os
import shutil
import wiredtiger, wttest
from helper import key_populate, simple_populate

# test_cache_pool.py
#    Checkpoint tests
# Test cache pool shared amongst multiple connections.
class test_cache_pool(wttest.WiredTigerTestCase):

    uri = 'table:test_cache_pool'
    # Setup fairly large items to use up cache
    data_str = 'abcdefghijklmnopqrstuvwxyz' * 20

    # Add a set of records
    def add_records(self, session, start, stop):
        cursor = session.open_cursor(self.uri, None, "overwrite")
        for i in range(start, stop+1):
            cursor.set_key("%010d KEY------" % i)
            cursor.set_value("%010d VALUE "% i + self.data_str)
            self.assertEqual(cursor.insert(), 0)
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
            pool_opts = ',cache=(pool=pool,size=200M,pool_chunk=20M,pool_min=60M),',
            extra_opts = '',
            add=0):
        if add == 0:
            self.conns = []
            self.sessions = []
        # Open the set of connections.
        for name in connections:
            shutil.rmtree(name, True)
            os.mkdir(name)
            next_conn =  wiredtiger.wiredtiger_open(
                name,
                'create,error_prefix="' + self.shortid() + ': "' +
                pool_opts + extra_opts)
            self.conns.append(next_conn)
            self.sessions.append(next_conn.open_session(None))
        return None

    def closeConnections(self):
        for tmp_conn in self.conns:
            tmp_conn.close()
        self.conns = []
        self.sessions = [] # Implicitly closed when closing sessions.

    # Basic test of cache pool
    def test_cache_pool1(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Test of cache pool with more connections
    def test_cache_pool2(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2', 'WT_TEST3', 'WT_TEST4'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Do enough work, so that the cache pool gets used.
    def test_cache_pool3(self):
        nops = 10000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])
        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")

        for i in range(20):
            for sess in self.sessions:
                self.add_records(sess, i * nops, (i + 1) * nops)
        self.closeConnections()

    # Switch the work between connections, to test rebalancing.
    def test_cache_pool4(self):
        # About 100 MB of data with ~250 byte values.
        nops = 200000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Add a new connection once the pool is already established.
    def test_cache_pool5(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)

        self.openConnections(['WT_TEST3'], add=1)
        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Close a connection and keep using other connections.
    def test_cache_pool6(self):
        nops = 10000
        self.openConnections(['WT_TEST1', 'WT_TEST2', 'WT_TEST3'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        conn = self.conns.pop()
        conn.close()
        self.sessions.pop()
        for sess in self.sessions:
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Test verbose output
    @unittest.skip("Verbose output handling")
    def test_cache_pool7(self):
        nops = 1000
        self.openConnections(
                ['WT_TEST1', 'WT_TEST2'], extra_opts="verbose=[cache_pool]")

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Test opening a connection outside of the cache pool
    def test_cache_pool8(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        self.openConnections(['WT_TEST3'], add=1, pool_opts=',cache=(size=50M)')
        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Test default config values
    def test_cache_pool9(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'], pool_opts=',cache=(pool=pool,size=200M)')

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

if __name__ == '__main__':
    wttest.run()
