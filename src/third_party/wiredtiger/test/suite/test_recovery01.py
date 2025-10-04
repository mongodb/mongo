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

import wttest
from helper import simulate_crash_restart
from wtdataset import SimpleDataSet
from wiredtiger import stat
from wtscenario import make_scenarios

# test_recovery01.py
# Test WiredTiger logs time spent during recovery and shutdown.
class test_recovery01(wttest.WiredTigerTestCase):

    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    restart_values = [
        ('crash', dict(crash=True)),
        ('shutdown', dict(crash=False))
    ]

    scenarios = make_scenarios(format_values, restart_values)

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RECOVERY_PROGRESS')

    def conn_config(self):
        config = 'cache_size=50MB,statistics=(all),log=(enabled=true),verbose=(recovery_progress)'
        return config

    def large_updates(self, uri, value, ds, nrows, commit_ts):
        # Update a large number of records.
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows+1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            if commit_ts == 0:
                session.commit_transaction()
            else:
                session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def check(self, check_value, uri, nrows, read_ts):
        session = self.session
        if read_ts == 0:
            session.begin_transaction()
        else:
            session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        cursor = session.open_cursor(uri)
        count = 0
        for k, v in cursor:
            self.assertEqual(v, check_value)
            count += 1
        session.commit_transaction()
        self.assertEqual(count, nrows)

    def test_recovery(self):
        nrows = 1000

        # Create two tables. One logged and another one non-logged.
        uri_1 = "table:recovery01_1"
        ds_1 = SimpleDataSet(
            self, uri_1, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=true)')
        ds_1.populate()

        uri_2 = "table:recovery01_2"
        ds_2 = SimpleDataSet(
            self, uri_2, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds_2.populate()

        if self.value_format == '8t':
            valuea = 97
            valueb = 98
        else:
            valuea = "aaaaa" * 100
            valueb = "bbbbb" * 100

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        self.large_updates(uri_1, valuea, ds_1, nrows, 0)
        self.check(valuea, uri_1, nrows, 0)

        self.large_updates(uri_2, valuea, ds_2, nrows, 10)
        self.check(valuea, uri_2, nrows, 10)

        self.large_updates(uri_1, valueb, ds_1, nrows, 0)
        self.check(valueb, uri_1, nrows, 0)

        self.large_updates(uri_2, valueb, ds_2, nrows, 20)
        self.check(valueb, uri_2, nrows, 20)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        if self.crash:
            simulate_crash_restart(self, ".", "RESTART")
        else:
            self.reopen_conn()

        self.check(valueb, uri_1, nrows, 0)
        self.check(valuea, uri_2, nrows, 10)
        self.check(valuea, uri_2, nrows, 20)
