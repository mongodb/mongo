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
# test_timestamp29.py
#   Test commit timestamp is set correctly on updates done before the
#   first commit timestamp is set.

import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

class test_timestamp28(wttest.WiredTigerTestCase):
    key_format_values = [
        ('fix', dict(key_format='r', value_format='8t')),
        ('row', dict(key_format='i', value_format='i')),
        ('var', dict(key_format='r', value_format='i')),
    ]

    scenarios = make_scenarios(key_format_values)
    def test_timestamp29(self):
        uri = 'table:timestamp29'
        ds = SimpleDataSet(self, uri, 0, key_format=self.key_format, value_format=self.value_format)
        ds.populate()
        c = self.session.open_cursor(uri)

        value = 1
        self.session.begin_transaction()
        c[ds.key(1)] = value
        self.session.timestamp_transaction("commit_timestamp=" + self.timestamp_str(10))
        c[ds.key(2)] = value
        self.session.timestamp_transaction("commit_timestamp=" + self.timestamp_str(20))
        c[ds.key(3)] = value
        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(30))

        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(5))
        for i in [1, 2, 3]:
            if self.value_format == '8t':
                self.assertEqual(c[ds.key(i)], 0)
            else:
                c.set_key(ds.key(i))
                self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(10))
        self.assertEqual(c[ds.key(1)], value)
        self.assertEqual(c[ds.key(2)], value)
        if self.value_format == '8t':
            self.assertEqual(c[ds.key(3)], 0)
        else:
            c.set_key(ds.key(3))
            self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(20))
        self.assertEqual(c[ds.key(1)], value)
        self.assertEqual(c[ds.key(2)], value)
        self.assertEqual(c[ds.key(3)], value)
        self.session.rollback_transaction()

        self.session.begin_transaction("read_timestamp=" + self.timestamp_str(30))
        self.assertEqual(c[ds.key(1)], value)
        self.assertEqual(c[ds.key(2)], value)
        self.assertEqual(c[ds.key(3)], value)
        self.session.rollback_transaction()
