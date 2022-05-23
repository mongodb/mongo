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
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtscenario import make_scenarios

# test_alter05.py
#    Check the alter command succeeds even if the file is modified.
class test_alter05(TieredConfigMixin, wttest.WiredTigerTestCase):
    name = "alter05"
    tiered_storage_sources = gen_tiered_storage_sources()
    scenarios = make_scenarios(tiered_storage_sources)

    # Setup custom connection config.
    def conn_config(self):
        conf = self.tiered_conn_config()
        if conf != '':
            conf += ','
        conf += 'statistics=(all)'
        return conf

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def verify_metadata(self, metastr):
        c = self.session.open_cursor('metadata:', None, None)

        # We must find a file type entry for this object and its value
        # should contain the provided file meta string.
        c.set_key('file:' + self.name)
        self.assertNotEqual(c.search(), wiredtiger.WT_NOTFOUND)
        value = c.get_value()
        self.assertTrue(value.find(metastr) != -1)

        c.close()

    # Alter file to change the metadata and verify.
    def test_alter05(self):
        uri = "file:" + self.name
        entries = 100
        create_params = 'key_format=i,value_format=i,'

        self.session.create(uri, create_params + "log=(enabled=true)")

        # Pin oldest and stable to timestamp 1.
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))

        # Verify the string in the metadata.
        self.verify_metadata('log=(enabled=true)')

        # Put some data in table.
        self.session.begin_transaction()
        c = self.session.open_cursor(uri, None)
        for k in range(entries):
            c[k+1] = 1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        c.close()

        prev_alter_checkpoints = self.get_stat(wiredtiger.stat.conn.session_table_alter_trigger_checkpoint)

        # Alter the table and verify.
        self.session.alter(uri, 'log=(enabled=false)')
        self.verify_metadata('log=(enabled=false)')

        alter_checkpoints = self.get_stat(wiredtiger.stat.conn.session_table_alter_trigger_checkpoint)
        self.assertEqual(prev_alter_checkpoints + 1, alter_checkpoints)
        prev_alter_checkpoints = alter_checkpoints

        # Open a cursor, insert some data and try to alter with cursor open.
        c = self.session.open_cursor(uri, None)
        self.session.begin_transaction()
        for k in range(entries):
            c[k+1] = 2
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))

        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.alter(uri, 'log=(enabled=true)'))
        self.verify_metadata('log=(enabled=false)')

        alter_checkpoints = self.get_stat(wiredtiger.stat.conn.session_table_alter_trigger_checkpoint)
        self.assertEqual(prev_alter_checkpoints + 1, alter_checkpoints)

if __name__ == '__main__':
    wttest.run()
