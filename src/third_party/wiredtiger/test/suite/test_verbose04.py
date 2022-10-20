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

from test_verbose01 import test_verbose_base
from wtdataset import SimpleDataSet
import wttest

# test_verbose04.py
# Verify extended debug verbose levels (WT_VERBOSE_DEBUG_2 through 5).
class test_verbose04(test_verbose_base):
    def updates(self, uri, value, ds, nrows, commit_ts):
        session = self.session
        cursor = session.open_cursor(uri)
        for i in range(1, nrows + 1):
            session.begin_transaction()
            cursor[ds.key(i)] = value
            session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))
        cursor.close()

    def test_verbose_level_2(self):
        self.close_conn()

        self.cleanStdout()
        verbose_config = self.create_verbose_configuration(['rts:2'])
        conn = self.wiredtiger_open(self.home, verbose_config)
        session = conn.open_session()

        self.conn = conn
        self.session = session

        uri = "table:test_verbose04"
        create_params = 'key_format=i,value_format=S'
        session.create(uri, create_params)

        ds = SimpleDataSet(self, uri, 0, key_format='i', value_format="S")
        ds.populate()

        nrows = 1000
        value = 'x' * 1000

        # Insert values with varying timestamps.
        self.updates(uri, value, ds, nrows, 20)

        # Move the stable timestamp past our updates.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(40))

        # Update values.
        self.updates(uri, value, ds, nrows, 60)

        # Perform a checkpoint and close the connection.
        self.session.checkpoint('use_timestamp=true')
        conn.close()

        output = self.readStdout(self.nlines)
        self.assertTrue('DEBUG_2' in output)
        self.cleanStdout()

    def walk_at_ts(self, check_value, uri, read_ts):
        session = self.session
        session.begin_transaction()
        cursor = session.open_cursor(uri)
        for k, v in cursor:
            pass
        session.commit_transaction()
        cursor.close()

    def test_verbose_level_3(self):
        self.close_conn()

        self.cleanStdout()
        verbose_config = self.create_verbose_configuration(['rts:3'])
        conn = self.wiredtiger_open(self.home, 'cache_size=50MB,' + verbose_config)
        session = conn.open_session()

        self.conn = conn
        self.session = session

        nrows = 1000

        # Create a table.
        uri = "table:test_verbose_04"
        ds = SimpleDataSet(self, uri, 0, key_format='i', value_format='S')
        ds.populate()

        value = "aaaaa" * 100

        # Pin the stable timestamp to 10.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))

        # Perform several updates.
        self.updates(uri, value, ds, nrows, 20)
        self.updates(uri, value, ds, nrows, 30)
        self.updates(uri, value, ds, nrows, 40)
        self.updates(uri, value, ds, nrows, 50)

        # Touch all of our data with a read timestamp > the commit timestamp.
        self.walk_at_ts(value, uri, 21)
        self.walk_at_ts(value, uri, 31)
        self.walk_at_ts(value, uri, 41)
        self.walk_at_ts(value, uri, 51)

        # Perform a checkpoint and close the connection.
        self.session.checkpoint()
        conn.close()

        # Possibly a lot of output here, allow many more chars than nlines.
        output = self.readStdout(self.nlines * 100000)
        self.assertTrue('DEBUG_3' in output)
        self.cleanStdout()

    def test_verbose_level_4_and_5(self):
        self.close_conn()

        self.cleanStdout()
        verbose_config = self.create_verbose_configuration(['recovery:5'])
        conn = self.wiredtiger_open(self.home, verbose_config)
        session = conn.open_session()

        ckpt_uri = 'table:test_verbose_04'
        session.create(ckpt_uri, 'key_format=i,value_format=i,log=(enabled=false)')
        c_ckpt = session.open_cursor(ckpt_uri)

        # Add some data.
        session.begin_transaction()
        c_ckpt[1] = 1
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Set the stable timestamp before the data.
        conn.set_timestamp('stable_timestamp=' + self.timestamp_str(9))

        # Run recovery.
        conn.close()
        conn = self.wiredtiger_open(self.home, verbose_config)

        output = self.readStdout(self.nlines)
        self.assertTrue('DEBUG_4' in output)
        self.assertTrue('DEBUG_5' in output)
        conn.close()
        self.cleanStdout()

if __name__ == '__main__':
    wttest.run()
