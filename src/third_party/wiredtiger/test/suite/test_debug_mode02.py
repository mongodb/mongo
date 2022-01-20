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

from suite_subprocess import suite_subprocess
import fnmatch, os, time, wiredtiger, wttest

# test_debug_mode02.py
#    Test the debug mode settings. Test checkpoint_retention use.
class test_debug_mode02(wttest.WiredTigerTestCase, suite_subprocess):
    uri = 'file:test_debug'

    entries = 100
    loop = 0
    retain = 5
    log1 = 'WiredTigerLog.0000000001'
    log2 = 'WiredTigerLog.0000000002'

    def conn_config(self):
        return 'log=(enabled=true,file_max=100K),debug_mode=(checkpoint_retention=%d)' % self.retain

    def log_set(self):
        logs = fnmatch.filter(os.listdir(self.home), "*gerLog*")
        return set(logs)

    def check_remove(self, logfile):
        removed = False
        for i in range(1,90):
            # Sleep and then see if log removal ran. We do this in a loop
            # for slow machines. Max out at 90 seconds.
            time.sleep(1.0)
            if not os.path.exists(logfile):
                removed = True
                break
        self.assertTrue(removed)

    def advance_log_checkpoint(self):
        # Advance the log file to the next file and write a checkpoint.
        keys = range(1, self.entries)
        cur_set = self.log_set()
        c = self.session.open_cursor(self.uri, None)
        new_set = cur_set
        # Write data in small chunks until we switch log files.
        while cur_set == new_set:
            for k in keys:
                c[k + (self.loop * self.entries)] = 1
            self.loop += 1
            new_set = self.log_set()
        c.close()
        # Write a checkpoint into the new log file.
        self.session.checkpoint()

    def test_checkpoint_retain(self):
        self.session.create(self.uri, 'key_format=i,value_format=i')
        # No log files should be removed while we have fewer than the
        # retention number of logs. Make sure each iteration the new
        # logs are a proper superset of the previous time.
        for i in range(1, self.retain):
            cur_set = self.log_set()
            self.advance_log_checkpoint()
            # We don't accomodate slow machines here because we don't expect
            # the files the change and there is no way to know if log removal ran
            # otherwise.
            time.sleep(1.0)
            new_set = self.log_set()
            self.assertTrue(new_set.issuperset(cur_set))

        self.assertTrue(os.path.exists(self.log1))
        self.advance_log_checkpoint()
        self.check_remove(self.log1)
        self.conn.reconfigure("debug_mode=(table_logging=true)")
        self.conn.reconfigure("verbose=(temporary)")

    # Test that both zero and one remove as usual. And test reconfigure.
    def test_checkpoint_retain_reconfig(self):
        # We can turn checkpoint retention off.
        # We can turn checkpoint retention on to some value.
        # We can reconfigure checkpoint retention to the same value.
        # We cannot reconfigure checkpoint retention to another value.
        # We can turn checkpoint retention off again.
        # We can reconfigure checkpoint retention to the original value.
        self.conn.reconfigure("debug_mode=(checkpoint_retention=0)")
        self.session.create(self.uri, 'key_format=i,value_format=i')
        self.advance_log_checkpoint()
        self.check_remove(self.log1)

        cfg = 'debug_mode=(checkpoint_retention=%d)' % self.retain
        self.conn.reconfigure(cfg)
        self.advance_log_checkpoint()
        self.assertTrue(os.path.exists(self.log2))

        self.conn.reconfigure(cfg)
        self.advance_log_checkpoint()
        self.assertTrue(os.path.exists(self.log2))

        msg = '/Cannot change value/'
        cfg1 = 'debug_mode=(checkpoint_retention=%d)' % (self.retain - 1)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure(cfg1), msg)

        self.conn.reconfigure("debug_mode=(checkpoint_retention=0)")
        self.advance_log_checkpoint()
        self.check_remove(self.log2)

        self.conn.reconfigure(cfg)

if __name__ == '__main__':
    wttest.run()
