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

import time
from helper import copy_wiredtiger_home
import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios
from wiredtiger import stat
from helper import simulate_crash_restart
from test_rollback_to_stable01 import test_rollback_to_stable_base

# Test that rollback to stable does not open any dhandles that don't have unstable updates.
class test_rollback_to_stable20(test_rollback_to_stable_base):
    session_config = 'isolation=snapshot'

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('integer_row', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all)'
        return config

    def test_rollback_to_stable(self):
        nrows = 10000
        ntables = 100
        create_params = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        uri = "table:rollback_to_stable20"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds.populate()

        if self.value_format == '8t':
            valuea = 97
        else:
            valuea = "aaaaa" * 100

        # Pin oldest and stable timestamp to 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        for i in range(0, ntables):
            uri = 'table:rollback_to_stable20_' + str(i)
            self.session.create(uri, create_params)
            self.large_updates(uri, valuea, ds, nrows, 0, 10)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        self.session.checkpoint()

        # Simulate a server crash and restart.
        self.pr("restart")
        simulate_crash_restart(self, ".", "RESTART")
        self.pr("restart complete")

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        open_dhandle_count = stat_cursor[stat.conn.dh_conn_handle_count][2]
        stat_cursor.close()

        self.assertLess(open_dhandle_count, 5)

if __name__ == '__main__':
    wttest.run()
