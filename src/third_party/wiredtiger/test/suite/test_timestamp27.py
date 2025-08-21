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
# test_timestamp27.py
#   Test rollback timestamp api
#

import wiredtiger, wttest
from wtscenario import make_scenarios

class test_timestamp27_preserve_prepared_off(wttest.WiredTigerTestCase):
    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    scenarios = make_scenarios(types)

    def test_non_prepared(self):
        self.session.begin_transaction()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(100)),
            '/rollback timestamp is set for an non-prepared transaction/')

    def test_prepared(self):
        self.session.begin_transaction()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50))
        self.session.timestamp_transaction('rollback_timestamp=' + self.timestamp_str(100))

class test_timestamp27_preserve_prepared_on(wttest.WiredTigerTestCase):
    conn_config = "precise_checkpoint=true,preserve_prepared=true"

    types = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='S', value_format='S')),
        ('var', dict(key_format='r', value_format='S')),
    ]
    scenarios = make_scenarios(types)

    def test_non_prepared(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.begin_transaction()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
        self.session.rollback_transaction('rollback_timestamp=' + self.timestamp_str(100)),
            '/rollback timestamp is set for an non-prepared transaction/')

    def test_prepared(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.begin_transaction()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50)+',prepared_id=123')
        self.session.timestamp_transaction('rollback_timestamp=' + self.timestamp_str(100))

    def test_rollback_timestamp_lt_stable(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.begin_transaction()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50)+',prepared_id=123')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
        self.session.timestamp_transaction('rollback_timestamp=' + self.timestamp_str(90)),
            '/is not newer than the stable timestamp/')

    def test_rollback_timestamp_eq_stable(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.begin_transaction()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50)+',prepared_id=123')
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(100))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
        self.session.timestamp_transaction('rollback_timestamp=' + self.timestamp_str(100)),
            '/is not newer than the stable timestamp/')

    def test_rollback_timestamp_with_commit_timestamp(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.begin_transaction()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50)+',prepared_id=123')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
        self.session.timestamp_transaction('rollback_timestamp=' + self.timestamp_str(100) + ",commit_timestamp=" + self.timestamp_str(100)),
            '/commit timestamp and rollback timestamp should not be set together/')

    def test_rollback_timestamp_with_commit_timestamp(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.begin_transaction()
        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(50)+',prepared_id=123')
        self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(100))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
        self.session.timestamp_transaction('rollback_timestamp=' + self.timestamp_str(100) + ",durable_timestamp=" + self.timestamp_str(100)),
            '/commit timestamp and rollback timestamp should not be set together/')

    def test_roundup_prepare_timestamp(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
        self.session.begin_transaction("roundup_timestamps=(prepare=true)"),
            '/cannot round up prepare timestamp to the oldest timestamp when the preserve prepare config is on/')
