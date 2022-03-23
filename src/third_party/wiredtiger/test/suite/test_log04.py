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

import wiredtiger, wttest
from wtscenario import make_scenarios
from wtdataset import SimpleDataSet

# test_log04.py
#    Smoke test logging with timestamp configurations.
class test_log04(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled)'

    types = [
        ('col', dict(key_format='r',value_format='S')),
        ('fix', dict(key_format='r',value_format='8t')),
        ('row', dict(key_format='S',value_format='S')),
    ]
    ckpt = [
        ('ckpt', dict(ckpt=True)),
        ('no-ckpt', dict(ckpt=False)),
    ]
    scenarios = make_scenarios(types, ckpt)

    def check(self, cursor, read_ts, key, value):
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(read_ts))
        self.assertEqual(cursor[key], value)
        self.session.rollback_transaction()

    def test_logts(self):
        # Create logged and non-logged objects. The non-logged objects are in two versions, one is
        # updated with a commit timestamp and one is not. Update the logged and non-logged timestamp
        # tables in a transaction with a commit timestamp and confirm the timestamps only apply to
        # the non-logged object. Update the non-logged, non-timestamp table in a transaction without
        # a commit timestamp, and confirm timestamps are ignored.
        uri_log = 'table:test_logts.log'
        ds_log = SimpleDataSet(self, uri_log, 100,
            key_format=self.key_format, value_format=self.value_format)
        ds_log.populate()
        c_log = self.session.open_cursor(uri_log)

        uri_ts = 'table:test_logts.ts'
        ds_ts = SimpleDataSet(self, uri_ts, 100,
            key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds_ts.populate()
        c_ts = self.session.open_cursor(uri_ts)

        uri_nots = 'table:test_log04.nots'
        ds_nots = SimpleDataSet(self, uri_nots, 100,
            key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)')
        ds_nots.populate()
        c_nots = self.session.open_cursor(uri_nots)

        # Set oldest and stable timestamps to 10.
        self.conn.set_timestamp(
            'oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        key = ds_ts.key(10)
        value10 = ds_ts.value(10)

        # Confirm initial data at timestamp 10.
        self.check(c_log, 10, key, value10)
        self.check(c_ts, 10, key, value10)
        self.check(c_nots, 10, key, value10)

        # Update and then rollback.
        value50 = ds_ts.value(50)
        self.session.begin_transaction()
        c_log[key] = value50
        c_ts[key] = value50
        c_nots[key] = value50
        self.session.rollback_transaction()

        # Confirm data at time 10 and 20.
        self.check(c_log, 10, key, value10)
        self.check(c_ts, 10, key, value10)
        self.check(c_nots, 10, key, value10)
        self.check(c_log, 20, key, value10)
        self.check(c_ts, 20, key, value10)
        self.check(c_nots, 20, key, value10)

        # Update and then commit data at time 20.
        value55 = ds_ts.value(55)
        self.session.begin_transaction()
        c_log[key] = value55
        c_ts[key] = value55
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        self.session.begin_transaction()
        c_nots[key] = value55
        self.session.commit_transaction()

        # Confirm data at time 10 and 20.
        self.check(c_log, 10, key, value55)
        self.check(c_ts, 10, key, value10)
        self.check(c_nots, 10, key, value55)
        self.check(c_log, 20, key, value55)
        self.check(c_nots, 20, key, value55)

        # Update and then commit data at time 30.
        value60 = ds_ts.value(60)
        self.session.begin_transaction()
        c_log[key] = value60
        c_ts[key] = value60
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        self.session.begin_transaction()
        c_nots[key] = value60
        self.session.commit_transaction()

        # Confirm data at time 20 and 30
        self.check(c_log, 20, key, value60)
        self.check(c_ts, 20, key, value55)
        self.check(c_nots, 20, key, value60)
        self.check(c_log, 30, key, value60)
        self.check(c_ts, 30, key, value60)
        self.check(c_nots, 30, key, value60)

        # Close cursors before calling RTS.
        c_log.close()
        c_ts.close()
        c_nots.close()

        # Move the stable timestamp to 25. Checkpoint and rollback to a timestamp.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(25))
        if self.ckpt:
            self.session.checkpoint()
        self.conn.rollback_to_stable()

        c_log = self.session.open_cursor(uri_log)
        c_ts = self.session.open_cursor(uri_ts)
        c_nots = self.session.open_cursor(uri_nots)

        # Confirm data at time 20 and 30.
        self.check(c_log, 20, key, value60)
        self.check(c_ts, 20, key, value55)
        self.check(c_nots, 20, key, value60)
        self.check(c_log, 30, key, value60)
        self.check(c_ts, 30, key, value55)
        self.check(c_nots, 30, key, value60)

if __name__ == '__main__':
    wttest.run()
