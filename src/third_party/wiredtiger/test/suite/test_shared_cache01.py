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

# test_shared_cache01.py
#    Checkpoint tests
# Test shared cache shared amongst multiple connections.
class test_shared_cache01(wttest.WiredTigerTestCase):

    uri = 'table:test_shared_cache01'
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

    # Basic test of shared cache
    def test_shared_cache_basic(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Test of shared cache with more connections
    def test_shared_cache_more_connections(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2', 'WT_TEST3', 'WT_TEST4'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Do enough work for the shared cache to be fully allocated.
    def test_shared_cache_full(self):
        nops = 10000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])
        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")

        for i in range(20):
            for sess in self.sessions:
                self.add_records(sess, i * nops, (i + 1) * nops)
        self.closeConnections()

    # Switch the work between connections, to test rebalancing.
    def test_shared_cache_rebalance(self):
        # About 100 MB of data with ~250 byte values.
        nops = 200000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Add a new connection once the shared cache is already established.
    def test_shared_cache_late_join(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)

        self.openConnections(['WT_TEST3'], add=1)
        self.sessions[-1].create(self.uri, "key_format=S,value_format=S")
        for sess in self.sessions:
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Close a connection and keep using other connections.
    def test_shared_cache_leaving(self):
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

    # Opening a connection with absolute values for eviction config should fail
    def test_shared_cache_absolute_evict_config(self):
        nops = 1000
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.openConnections(['WT_TEST1', 'WT_TEST2'],
            pool_opts = ',shared_cache=(name=pool,size=50M,reserve=20M),'
            'eviction_target=10M,'), '/Shared cache configuration requires a '
            'percentage value for eviction target/')

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.openConnections(['WT_TEST1', 'WT_TEST2'],
            pool_opts = ',shared_cache=(name=pool,size=50M,reserve=20M),'
            'eviction_trigger=10M,'), '/Shared cache configuration requires a '
            'percentage value for eviction trigger/')

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.openConnections(['WT_TEST1', 'WT_TEST2'],
            pool_opts = ',shared_cache=(name=pool,size=50M,reserve=20M),'
            'eviction_dirty_target=10M,'), '/Shared cache configuration '
            'requires a percentage value for eviction dirty target/')

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.openConnections(['WT_TEST1', 'WT_TEST2'],
            pool_opts = ',shared_cache=(name=pool,size=50M,reserve=20M),'
            'eviction_dirty_trigger=10M,'), '/Shared cache configuration '
            'requires a percentage value for eviction dirty trigger/')

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.openConnections(['WT_TEST1', 'WT_TEST2'],
            pool_opts = ',shared_cache=(name=pool,size=50M,reserve=20M),'
            'eviction_checkpoint_target=10M,'), '/Shared cache configuration '
            'requires a percentage value for eviction checkpoint target/')

    # Test verbose output
    def test_shared_cache_verbose(self):
        nops = 1000
        self.openConnections(
                ['WT_TEST1', 'WT_TEST2'], extra_opts="verbose=[shared_cache]")

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()
        self.captureout.checkAdditionalPattern(self, " *Created cache pool pool")

    # Test opening a connection outside of the shared cache
    def test_shared_cache_mixed(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'])

        self.openConnections(['WT_TEST3'], add=1, pool_opts=',cache_size=50M')
        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Test default config values
    def test_shared_cache_defaults(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'], pool_opts=',shared_cache=(name=pool,size=200M)')

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

    # Test default config values
    def test_shared_cache_defaults2(self):
        nops = 1000
        self.openConnections(['WT_TEST1', 'WT_TEST2'], pool_opts=',shared_cache=(name=pool)')

        for sess in self.sessions:
            sess.create(self.uri, "key_format=S,value_format=S")
            self.add_records(sess, 0, nops)
        self.closeConnections()

if __name__ == '__main__':
    wttest.run()
