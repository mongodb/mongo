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

from wtscenario import make_scenarios
import wiredtiger, wttest

# test_checkpoint31.py
#
# Test opening a read-only checkpoint cursor.
@wttest.skip_for_hook("disagg", "layered trees do not support named checkpoints")
class test_checkpoint(wttest.WiredTigerTestCase):
    ckpt_precision = [
        ('fuzzy', dict(ckpt_config='precise_checkpoint=false')),
        ('precise', dict(ckpt_config='precise_checkpoint=true')),
    ]
    scenarios = make_scenarios(ckpt_precision)

    def conn_config(self):
        return self.ckpt_config

    def test_checkpoint(self):
        uri = 'table:checkpoint31'

        # Create a table with some initial data.
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri, None, None)

        self.session.begin_transaction()
        cursor['1'] = '10'
        cursor['2'] = '20'
        self.session.prepare_transaction('prepare_timestamp=10')
        self.session.commit_transaction('commit_timestamp=14,durable_timestamp=18')

        self.session.begin_transaction()
        cursor['2'] = '21'
        self.session.prepare_transaction('prepare_timestamp=20')
        self.session.commit_transaction('commit_timestamp=24,durable_timestamp=28')

        # Create a named checkpoint between the commit and the durable timestamps of the last
        # transaction.
        self.conn.set_timestamp('stable_timestamp=26')
        self.session.checkpoint('name=ckpt1')

        # Now take a nameless checkpoint with the latest stable timestamp.
        self.conn.set_timestamp('stable_timestamp=40')
        self.session.checkpoint()

        # Reopen the connection and confirm the value of key 2
        self.reopen_conn()
        cursor = self.session.open_cursor(uri, None, 'checkpoint=ckpt1')
        cursor.set_key('2')
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), '20')

        # Now try with a read-only connection
        self.reopen_conn(config='readonly=true')
        cursor = self.session.open_cursor(uri, None, 'checkpoint=ckpt1')
        cursor.set_key('2')
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), '20')
