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
# test_txn14.py
#   Transactions: commits and rollbacks
#

import fnmatch, os, shutil, time
from helper import simulate_crash_restart
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios
import wttest

class test_txn14(wttest.WiredTigerTestCase, suite_subprocess):
    t1 = 'table:test_txn14_1'
    entries = 10000
    extra_entries = 5
    conn_config = 'log=(archive=false,enabled,file_max=100K)'

    sync_list = [
        ('write', dict(sync='off')),
        ('sync', dict(sync='on')),
    ]
    format_values = [
        ('integer-row', dict(key_format='i', value_format='i')),
        ('column', dict(key_format='r', value_format='i')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    scenarios = make_scenarios(sync_list, format_values)

    def mkvalue(self, i):
        if self.value_format == '8t':
            # Use 255 instead of 256 just for variety.
            return i % 255
        return i

    def test_log_flush(self):
        # Here's the strategy:
        #    - Create a table.
        #    - Insert data into table.
        #    - Call log_flush.
        #    - Simulate a crash and restart
        #    - Make recovery run.
        #    - Confirm flushed data is in the table.
        #
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.t1, create_params)
        c = self.session.open_cursor(self.t1, None, None)
        for i in range(1, self.entries + 1):
            c[i] = self.mkvalue(i + 1)
        cfgarg='sync=%s' % self.sync
        self.pr('cfgarg ' + cfgarg)
        self.session.log_flush(cfgarg)
        for i in range(1, self.extra_entries + 1):
            c[i+self.entries] = self.mkvalue(i + self.entries + 1)
        c.close()
        self.session.log_flush(cfgarg)
        simulate_crash_restart(self, ".", "RESTART")
        c = self.session.open_cursor(self.t1, None, None)
        i = 1
        for key, value in c:
            self.assertEqual(i, key)
            self.assertEqual(self.mkvalue(i+1), value)
            i += 1
        all = self.entries + self.extra_entries
        self.assertEqual(i, all + 1)
        c.close()

if __name__ == '__main__':
    wttest.run()
