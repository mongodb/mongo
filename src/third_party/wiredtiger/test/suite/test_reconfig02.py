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
# [TEST_TAGS]
# connection_api:reconfigure
# [END_TAGS]

import fnmatch, os, time
import wiredtiger, wttest

# test_reconfig02.py
#    Smoke-test the connection reconfiguration operations.
class test_reconfig02(wttest.WiredTigerTestCase):
    init_config = 'log=(enabled,file_max=100K,prealloc=false,remove=false,zero_fill=false)'
    uri = "table:reconfig02"
    entries = 1000

    def setUpConnectionOpen(self, dir):
        self.conn_config = self.init_config
        return wttest.WiredTigerTestCase.setUpConnectionOpen(self, dir)

    # Logging: reconfigure the things we can reconfigure.
    def test_reconfig02_simple(self):
        self.conn.reconfigure("log=(remove=false)")
        self.conn.reconfigure("log=(prealloc=false)")
        self.conn.reconfigure("log=(zero_fill=false)")

        self.conn.reconfigure("log=(remove=true)")
        self.conn.reconfigure("log=(prealloc=true)")
        self.conn.reconfigure("log=(zero_fill=true)")

    # Logging: reconfigure the things we can't reconfigure.
    def test_reconfig02_disable(self):
        msg = '/unknown configuration key/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure("log=(enabled=true)"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure("log=(compressor=foo)"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure("log=(file_max=1MB)"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure("log=(path=foo)"), msg)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure("log=(recover=true)"), msg)

    # Logging starts on, but prealloc is off.  Verify it is off.
    # Reconfigure it on and run again, making sure that log files
    # get pre-allocated.
    def test_reconfig02_prealloc(self):
        # Create a table just to write something into the log.  Sleep
        # to give the worker thread a chance to run.
        self.session.create(self.uri, 'key_format=i,value_format=i')
        time.sleep(2)
        prep_logs = fnmatch.filter(os.listdir('.'), "*Prep*")
        # Make sure no pre-allocated log files exist.
        self.assertEqual(0, len(prep_logs))

        # Now turn on pre-allocation.  Sleep to give the worker thread
        # a chance to run and verify pre-allocated log files exist.
        #
        # Potentially loop a few times in case it is a very slow system.
        self.conn.reconfigure("log=(prealloc=true)")
        for x in range(0, 100):
            time.sleep(1)
            prep_logs = fnmatch.filter(os.listdir('.'), "*Prep*")
            if len(prep_logs) != 0:
                break

        self.assertNotEqual(0, len(prep_logs))

    # Logging starts on, but remove is off.  Verify it is off.
    # Reconfigure it on and run again, making sure that log files
    # get removed.
    def test_reconfig02_remove(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri, None, None)
        for i in range(self.entries):
            c[i] = i + 1
        c.close()
        # Close and reopen connection to write a checkpoint, move to the
        # next log file and verify that removal did not run.
        orig_logs = fnmatch.filter(os.listdir('.'), "*gerLog*")
        self.reopen_conn()
        cur_logs = fnmatch.filter(os.listdir('.'), "*gerLog*")
        for o in orig_logs:
            self.assertEqual(True, o in cur_logs)

        # Now turn on removal, sleep a bit to allow the removal thread
        # to run and then confirm that all original logs are gone.
        self.conn.reconfigure("log=(remove=true)")
        self.session.checkpoint("force")
        time.sleep(2)
        cur_logs = fnmatch.filter(os.listdir('.'), "*gerLog*")
        for o in orig_logs:
            self.assertEqual(False, o in cur_logs)

if __name__ == '__main__':
    wttest.run()
