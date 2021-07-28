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

import fnmatch, os, time
import wiredtiger, wttest
from wtscenario import make_scenarios

# test_config07.py
#    Test that log files extend as configured and as documented.
class test_config07(wttest.WiredTigerTestCase):
    uri = "table:test"
    entries = 5000
    K = 1024
    log_size = K * K

    extend_len = [
        ('default', dict(log_extend_len='()', expected_log_size = log_size)),
        ('empty', dict(log_extend_len='(log=)', expected_log_size = log_size)),
        ('disable', dict(log_extend_len='(log=0)', expected_log_size = 128)),
        ('100K', dict(log_extend_len='(log=100K)', expected_log_size = 100 * K)),
        ('too_small', dict(log_extend_len='(log=20K)', expected_log_size = None)),
        ('too_large', dict(log_extend_len='(log=20G)', expected_log_size = None)),
        ('small_in_allowed range', dict(log_extend_len='(log=200K)',
                                       expected_log_size = 200 * K)),
        ('large_in_allowed_range', dict(log_extend_len='(log=900K)',
                                       expected_log_size = 900 * K)),
        ('larger_than_log_file_size', dict(log_extend_len='(log=20M)',
                                       expected_log_size = log_size)),
        ('with_data_file_extend_conf', dict(log_extend_len='(log=100K,data=16M)',
                                       expected_log_size = 100 * K)),
    ]

    scenarios = make_scenarios(extend_len)

    def populate(self):
        cur = self.session.open_cursor(self.uri, None, None)
        for i in range(0, self.entries):
            # Make the values about 200 bytes. That's about 1MB of data for
            # 5000 records, generating 10 log files used plus more for overhead.
            cur[i] = "abcde" * 40
        cur.close()

    def checkLogFileSize(self, size):
        # Wait for a log file to be preallocated. Avoid timing problems, but
        # assert that a file is created within 1 minute.
        for i in range(1,60):
            logs = fnmatch.filter(os.listdir('.'), "*Prep*")
            if logs:
                f = logs[-1]
                file_size = os.stat(f).st_size
                self.assertEqual(size, file_size)
                break
            time.sleep(1)
        self.assertTrue(logs)

    def test_log_extend(self):
        self.conn.close()
        msg = '/invalid log extend length/'

        config = 'log=(enabled,file_max=1M),file_extend=' + self.log_extend_len
        configarg = 'create,' + config

        # Expect an error when an invalid log extend size is provided.
        if self.expected_log_size is None:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.wiredtiger_open('.', configarg), msg)
            return

        self.conn = self.wiredtiger_open('.', configarg)
        self.session = self.conn.open_session(None)

        # Create a table, insert data in it to trigger log file writes.
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.populate()
        self.session.checkpoint()

        self.checkLogFileSize(self.expected_log_size)

if __name__ == '__main__':
    wttest.run()
