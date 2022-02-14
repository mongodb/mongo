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

import os
import wiredtiger
from wtscenario import make_scenarios
from test_rollback_to_stable01 import test_rollback_to_stable_base

# test_rollback_to_stable09.py
# Test that rollback to stable does not abort schema operations that are done
# as they don't have transaction support
class test_rollback_to_stable09(test_rollback_to_stable_base):

    # Don't bother testing FLCS tables as well as they're highly unlikely to
    # behave differently at this level.
    colstore_values = [
        ('column', dict(use_columns=True)),
        ('row', dict(use_columns=False)),
    ]

    in_memory_values = [
        ('no_inmem', dict(in_memory=False)),
        ('inmem', dict(in_memory=True))
    ]

    prepare_values = [
        ('no_prepare', dict(prepare=False)),
        ('prepare', dict(prepare=True))
    ]

    tablename = "test_rollback_stable09"
    uri = "table:" + tablename
    index_uri = "index:test_rollback_stable09:country"

    scenarios = make_scenarios(colstore_values, in_memory_values, prepare_values)

    def conn_config(self):
        config = 'cache_size=250MB'
        if self.in_memory:
            config += ',in_memory=true'
        else:
            config += ',log=(enabled),in_memory=false'
        return config

    def create_table(self, commit_ts):
        self.pr('create table')
        session = self.session
        session.begin_transaction()
        if self.use_columns:
                config = 'key_format=r,value_format=5sHQ,columns=(id,country,year,population)'
        else:
                config = 'key_format=5s,value_format=HQ,columns=(country,year,population)'
        session.create(self.uri, config)
        if commit_ts == 0:
                session.commit_transaction()
        elif self.prepare:
            session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(commit_ts-1))
            session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(commit_ts+1))
            session.commit_transaction()
        else:
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def drop_table(self, commit_ts):
        self.pr('drop table')
        session = self.session
        session.begin_transaction()
        session.drop(self.uri)
        if commit_ts == 0:
                session.commit_transaction()
        elif self.prepare:
            session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(commit_ts-1))
            session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(commit_ts+1))
            session.commit_transaction()
        else:
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def create_index(self, commit_ts):
        session = self.session
        session.begin_transaction()
        self.session.create(self.index_uri, "key_format=s,columns=(country)")
        if commit_ts == 0:
                session.commit_transaction()
        elif self.prepare:
            session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(commit_ts-1))
            session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
            session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(commit_ts+1))
            session.commit_transaction()
        else:
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def test_rollback_to_stable(self):
        # Pin oldest and stable to timestamp 10.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))

        # Create table and index at a later timestamp
        self.create_table(20)
        self.create_index(30)

        #perform rollback to stable, still the table and index must exist
        self.conn.rollback_to_stable()

        if not self.in_memory:
            self.assertTrue(os.path.exists(self.tablename + ".wt"))
            self.assertTrue(os.path.exists(self.tablename + "_country.wti"))

        # Check that we are able to open cursor successfully on the table and index
        c = self.session.open_cursor(self.uri, None, None)
        self.assertTrue(c.next(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(c.close(), 0)

        c = self.session.open_cursor(self.index_uri, None, None)
        self.assertTrue(c.next(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(c.close(), 0)

        # Drop the table
        self.drop_table(40)

        #perform rollback to stable, the table and index must not exist
        self.conn.rollback_to_stable()

        if not self.in_memory:
            self.assertFalse(os.path.exists(self.tablename + ".wt"))
            self.assertFalse(os.path.exists(self.tablename + "_country.wti"))

        # Check that we are unable to open cursor on the table and index
        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            self.session.open_cursor(self.uri, None, None))
        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            self.session.open_cursor(self.index_uri, None, None))

if __name__ == '__main__':
    wttest.run()
