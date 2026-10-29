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

# test_layered_config16.py
#    A database whose shared tree metadata entries lack the disaggregated block manager, as
#    written by earlier versions, behaves like one whose entries have it.

import re, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_config16(wttest.WiredTigerTestCase):
    conn_config = 'precise_checkpoint=true,disaggregated=(role="leader")'
    follower_config = 'precise_checkpoint=true,disaggregated=(role="follower")'
    uri = 'layered:test_layered_config16'
    shared_metadata_uri = 'file:WiredTigerShared.wt_stable'
    stable_uri = 'file:test_layered_config16.wt_stable'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def local_config(self, uri):
        cursor = self.session.open_cursor('file:WiredTiger.wt')
        cursor.set_key(uri)
        self.assertEqual(cursor.search(), 0)
        value = cursor.get_value()
        cursor.close()
        return value

    def make_legacy(self, uri):
        value = self.local_config(uri)
        self.assertIn('block_manager=disagg', value)
        cursor = self.session.open_cursor('file:WiredTiger.wt')
        cursor.set_key(uri)
        cursor.set_value(value.replace('block_manager=disagg', 'block_manager=default'))
        self.assertEqual(cursor.insert(), 0)
        cursor.close()
        self.assertNotIn('block_manager=disagg', self.local_config(uri))

    def populate_table(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor['key'] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        cursor.close()
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

    def populate(self):
        self.populate_table()
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()

        # A follower does not checkpoint, so the rewritten entries survive the restart.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.make_legacy(self.shared_metadata_uri)
        self.make_legacy(self.stable_uri)
        return checkpoint_meta

    def check_read(self):
        cursor = self.session.open_cursor(self.uri)
        self.assertEqual(cursor['key'], 'value')
        cursor.close()

    def test_legacy_leader(self):
        self.populate()
        self.reopen_conn()
        self.check_read()
        self.session.checkpoint()
        self.assertEqual(self.session.verify(self.shared_metadata_uri), 0)
        self.assertEqual(self.session.verify(self.stable_uri), 0)

    def test_legacy_follower(self):
        checkpoint_meta = self.populate()
        self.reopen_conn(config=self.follower_config)
        self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        self.check_read()

    def test_new_shared_files_have_disagg_block_manager(self):
        self.populate_table()
        self.session.create('file:test_layered_config16_plain.wt_stable',
            'key_format=S,value_format=S')
        cursor = self.session.open_cursor('file:WiredTiger.wt')
        shared = {}
        while cursor.next() == 0:
            key = cursor.get_key()
            if key.startswith('file:') and key.endswith('.wt_stable'):
                shared[key] = cursor.get_value()
        cursor.close()

        for uri in (self.shared_metadata_uri, self.stable_uri,
          'file:test_layered_config16_plain.wt_stable', 'file:WiredTigerSharedHS.wt_stable'):
            self.assertIn(uri, shared)
        for uri, value in shared.items():
            self.assertIn('block_manager=disagg', value, uri)

    def test_config_only_shared_checkpoint_view(self):
        uri = 'file:test_layered_config16_plain.wt'
        self.session.create(uri, 'key_format=S,value_format=S,block_manager=disagg')
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        cursor['key'] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))
        cursor.close()
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
            ',stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

        # A shared file without a ".wt_stable" name is read at a checkpoint by its configuration.
        name = re.search(r'WiredTigerCheckpoint\.\d+', self.local_config(uri)).group(0)
        cursor = self.session.open_cursor(uri + '/' + name)
        self.assertEqual(cursor['key'], 'value')
        cursor.close()
