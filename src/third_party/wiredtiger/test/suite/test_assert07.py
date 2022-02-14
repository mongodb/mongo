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
# test_assert07.py
#   Verify that the resolved update assertion does not get triggerd by having
#   reserved updates at different locations in the update chain.
#

from suite_subprocess import suite_subprocess
import wttest
from wtscenario import make_scenarios

class test_assert07(wttest.WiredTigerTestCase, suite_subprocess):
    key_format_values = [
        ('column', dict(key_format='r', usestrings=False)),
        ('string-row', dict(key_format='S', usestrings=True))
    ]
    scenarios = make_scenarios(key_format_values)

    def apply_timestamps(self, timestamp):
        self.session.prepare_transaction(
            'prepare_timestamp=' + self.timestamp_str(timestamp))
        self.session.timestamp_transaction(
            'commit_timestamp=' + self.timestamp_str(timestamp))
        self.session.timestamp_transaction(
            'durable_timestamp=' + self.timestamp_str(timestamp))

    def test_timestamp_alter(self):
        base = 'assert07'
        uri = 'file:' + base

        key_ts1 = 'key_ts1' if self.usestrings else 1

        # No reserved, single update.
        self.session.create(uri, 'key_format={},value_format=S'.format(self.key_format))
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        c[key_ts1] = 'value1'
        self.apply_timestamps(1)
        self.session.commit_transaction()

        # Reserved at the start of the chain, with one update.
        self.session.begin_transaction()
        c.set_key(key_ts1)
        c.reserve()
        c[key_ts1] = 'value2'
        self.apply_timestamps(2)
        self.session.commit_transaction()

        # Reserved at the end of the chain, with one update.
        self.session.begin_transaction()
        c[key_ts1] = 'value3'
        c.set_key(key_ts1)
        c.reserve()
        self.apply_timestamps(3)
        self.session.commit_transaction()

        # Reserved at the start of the chain, with multiple.
        self.session.begin_transaction()
        c.set_key(key_ts1)
        c.reserve()
        c[key_ts1] = 'value4'
        c[key_ts1] = 'value5'
        self.apply_timestamps(4)
        self.session.commit_transaction()

        # Reserved at the end of the chain, with multiple updates.
        self.session.begin_transaction()
        c[key_ts1] = 'value6'
        c[key_ts1] = 'value7'
        c.set_key(key_ts1)
        c.reserve()
        self.apply_timestamps(5)
        self.session.commit_transaction()

        # Reserved between two updates.
        self.session.begin_transaction()
        c[key_ts1] = 'value8'
        c.set_key(key_ts1)
        c.reserve()
        c[key_ts1] = 'value9'
        self.apply_timestamps(6)
        self.session.commit_transaction()

        # Reserved update with multiple extra updates.
        self.session.begin_transaction()
        c[key_ts1] = 'value10'
        c.set_key(key_ts1)
        c.reserve()
        c[key_ts1] = 'value11'
        c[key_ts1] = 'value12'
        c[key_ts1] = 'value13'
        self.apply_timestamps(7)
        self.session.commit_transaction()

        # Reserved updates with multiple extra updates.
        self.session.begin_transaction()
        c[key_ts1] = 'value14'
        c.set_key(key_ts1)
        c.reserve()
        c[key_ts1] = 'value15'
        c[key_ts1] = 'value16'
        c.set_key(key_ts1)
        c.reserve()
        c[key_ts1] = 'value17'
        self.apply_timestamps(8)
        self.session.commit_transaction()

if __name__ == '__main__':
    wttest.run()
