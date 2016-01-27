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
#
# test_sweep03.py
# Test to confirm if setting close_idle_time to 0 does not sweep old handles
#

import fnmatch, os, shutil, run, time
from suite_subprocess import suite_subprocess
from wiredtiger import stat
from wtscenario import multiply_scenarios, number_scenarios, prune_scenarios
import wttest

class test_sweep03(wttest.WiredTigerTestCase, suite_subprocess):
    tablebase = 'test_sweep03'
    uri = 'table:' + tablebase
    numfiles = 400 # Make this more than the default close_handle_minimum
    numkv = 100
    conn_config = 'file_manager=(close_handle_minimum=10,' + \
                  'close_idle_time=0,close_scan_interval=1),' + \
                  'statistics=(fast),'

    types = [
        ('row', dict(tabletype='row',
                    create_params = 'key_format=i,value_format=i')),
        ('var', dict(tabletype='var',
                    create_params = 'key_format=r,value_format=i')),
        ('fix', dict(tabletype='fix',
                    create_params = 'key_format=r,value_format=8t')),
    ]

    scenarios = types

    def test_disable_idle_timeout1(self):
        #
        # Set up numfiles with numkv entries.  We just want some data in there
        # we don't care what it is.
        #
        for f in range(self.numfiles):
            uri = '%s.%d' % (self.uri, f)
            # print "Creating %s with config '%s'" % (uri, self.create_params)
            self.session.create(uri, self.create_params)
            c = self.session.open_cursor(uri, None)
            for k in range(self.numkv):
                c[k+1] = 1
            c.close()

        #
        # The idle timeout is disabled - we don't expect the sweep server to
        # close any regular handles. Give the sweep server 5x the scan
        # interval to close any handles.
        #
        time.sleep(5)

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        close1 = stat_cursor[stat.conn.dh_sweep_close][2]
        sweep1 = stat_cursor[stat.conn.dh_sweeps][2]
        stat_cursor.close()

        # The sweep server should have run, or the test isn't working.
        self.assertGreater(sweep1, 0)
        # We expect nothing to have been closed.
        self.assertEqual(close1, 0)

    def test_disable_idle_timeout_drop_force(self):
        # Create a table to drop. A drop should close its associated handle
        drop_uri = '%s.%s' % (self.uri, "force_drop_test")

        self.session.create(drop_uri, self.create_params)

        c = self.session.open_cursor(drop_uri, None)
        for k in range(self.numkv):
            c[k+1] = 1
        c.close()

        # We just filled the table, now check what the stats are
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cache1 = stat_cursor[stat.conn.cache_bytes_inuse][2]
        sweep1 = stat_cursor[stat.conn.dh_sweeps][2]
        stat_cursor.close()

        # We force the drop in this case to confirm that the handle is closed
        self.session.drop(drop_uri, "force=true")

        time.sleep(5)

        # Grab the stats post table drop to see things have decremented
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cache2 = stat_cursor[stat.conn.cache_bytes_inuse][2]
        close2 = stat_cursor[stat.conn.dh_sweep_close][2]
        sweep2 = stat_cursor[stat.conn.dh_sweeps][2]
        stat_cursor.close()

        # Make sure the sweep server is still working.
        self.assertGreater(sweep2, sweep1)
        # Ensure that the handle has been closed after the drop.
        self.assertEqual(close2, 1)
        # Ensure that any space was reclaimed from cache.
        self.assertLess(cache2, cache1)

    def test_disable_idle_timeout_drop(self):
        # Create a table to drop. A drop should close its associated handles
        drop_uri = '%s.%s' % (self.uri, "drop_test")
        self.session.create(drop_uri, self.create_params)

        c = self.session.open_cursor(drop_uri, None)
        for k in range(self.numkv):
            c[k+1] = 1
        c.close()

        # We just filled the table, now check what the stats are
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cache1 = stat_cursor[stat.conn.cache_bytes_inuse][2]
        close1 = stat_cursor[stat.conn.dh_sweep_close][2]
        sweep1 = stat_cursor[stat.conn.dh_sweeps][2]
        stat_cursor.close()

        self.session.drop(drop_uri, None)

        time.sleep(5)

        # Grab the stats post table drop to see things have decremented
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cache2 = stat_cursor[stat.conn.cache_bytes_inuse][2]
        close2 = stat_cursor[stat.conn.dh_sweep_close][2]
        sweep2 = stat_cursor[stat.conn.dh_sweeps][2]
        stat_cursor.close()

        self.assertGreater(sweep2, sweep1)
        # The sweep server should not be involved in regular drop cleanup
        self.assertEqual(close2, close1)
        # Ensure that any space was reclaimed from cache.
        self.assertLess(cache2, cache1)

if __name__ == '__main__':
    wttest.run()
