#!/usr/bin/env python3
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
from helper import WiredTigerCursor
from helper_disagg import disagg_test_class, DisaggSchemaEpochMixin, gen_disagg_storages
from metadata_helper import get_table_id
from wiredtiger import stat
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_schema37(DisaggSchemaEpochMixin, wttest.WiredTigerTestCase):
    conn_config = 'precise_checkpoint=true,disaggregated=(role="leader",lose_all_my_data=true)'
    conn_config_follower = 'precise_checkpoint=true,statistics=(all),disaggregated=(role="follower")'
    layered_uri = 'layered:test_layered_schema37'
    ingest_uri = 'file:test_layered_schema37.wt_ingest'

    creation_formats = [
        ('layered', dict(uri='layered:test_layered_schema37', extra_config='')),
        ('table', dict(uri='table:test_layered_schema37',
            extra_config=',type=layered,block_manager=disagg')),
    ]
    data_formats = [
        ('string', dict(key_format='S', value_format='S', key='key', value='value')),
        ('integer', dict(key_format='i', value_format='I', key=1, value=1)),
    ]
    scenarios = make_scenarios(gen_disagg_storages(disagg_only=True),
        creation_formats, data_formats)

    def table_config(self):
        return f'key_format={self.key_format},value_format={self.value_format}' + self.extra_config

    def create_populated_table(self, uri, timestamp=1):
        self.session.create(uri, self.table_config())
        with self.transaction(commit_timestamp=timestamp):
            with WiredTigerCursor(self.session, uri) as cursor:
                cursor[self.key] = self.value

    def create_table_and_start_follower(self):
        self.create_populated_table(self.uri)
        self.leader_checkpoint(1)
        self.conn_follow, self.session_follow = self.open_follower()

    def follower_metadata(self, uri):
        with WiredTigerCursor(self.session_follow, 'metadata:') as cursor:
            return cursor[uri]

    def follower_has_ingest_metadata(self, layered_uri):
        return self.uri_in_local_metadata(self.conn_follow, layered_uri)

    def lazy_ingest_creation_count(self):
        return self.get_stat(stat.conn.disagg_ingest_create_on_open, session=self.session_follow)

    def assert_ingest_open_fails(self, uri):
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session_follow.open_cursor(uri), '/No such file or directory/')
        with WiredTigerCursor(self.session_follow, 'metadata:') as cursor:
            cursor.set_key(uri)
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)

    # Opening a table creates ingest metadata that is reused by later opens and reopens, while
    # tables that are never opened have none.
    def test_ingest_creation_lifecycle(self):
        self.create_populated_table(self.uri + '_unopened')
        self.create_table_and_start_follower()
        self.assertFalse(self.follower_has_ingest_metadata(self.layered_uri))

        with WiredTigerCursor(self.session_follow, self.uri) as cursor:
            self.assertEqual(cursor[self.key], self.value)
        self.assertTrue(self.follower_has_ingest_metadata(self.layered_uri))

        config = self.follower_metadata(self.ingest_uri)
        for setting in (f'key_format="{self.key_format}"', f'value_format="{self.value_format}"',
            'in_memory=true', 'log=(enabled=false)'):
            self.assertIn(setting, config)
        with WiredTigerCursor(self.session_follow, 'file:WiredTigerShared.wt_stable') as cursor:
            self.assertNotIn(self.ingest_uri, dict(cursor))

        ingest_id = get_table_id(self.session_follow, self.ingest_uri)
        creations = self.lazy_ingest_creation_count()
        self.session_follow.open_cursor(self.uri).close()
        self.assertEqual(get_table_id(self.session_follow, self.ingest_uri), ingest_id)
        self.assertEqual(self.lazy_ingest_creation_count(), creations)

        self.close_follower(self.conn_follow, self.session_follow)
        self.conn_follow, self.session_follow = self.open_follower()
        self.session_follow.open_cursor(self.ingest_uri).close()
        self.assertEqual(get_table_id(self.session_follow, self.ingest_uri), ingest_id)
        self.assertEqual(self.lazy_ingest_creation_count(), 0)

        self.assertFalse(self.follower_has_ingest_metadata(self.layered_uri + '_unopened'))

    def test_ingest_creation_guards(self):
        self.create_populated_table(self.uri + '_other')
        self.create_table_and_start_follower()

        other_ingest_uri = self.ingest_uri.replace('.wt_ingest', '_other.wt_ingest')
        self.session_follow.open_cursor(other_ingest_uri).close()
        self.assertTrue(self.follower_has_ingest_metadata(self.layered_uri + '_other'))

        self.assert_ingest_open_fails('file:missing.wt_ingest')
        self.assert_ingest_open_fails('file:.wt_ingest')

    def test_ingest_creation_mismatched_parent(self):
        self.create_table_and_start_follower()
        config = self.follower_metadata(self.layered_uri) + ',ingest="file:other.wt_ingest"'
        self.inject_stable_entry(self.conn_follow, self.layered_uri, config)
        self.assert_ingest_open_fails(self.ingest_uri)

    def test_ingest_creation_dropped_parent(self):
        self.create_table_and_start_follower()
        self.session_follow.drop(self.uri)
        self.assert_ingest_open_fails(self.ingest_uri)

    # Rolling back the transaction that first opened the table does not remove the created ingest
    # metadata.
    def test_transaction_rollback_preserves_created_ingest(self):
        self.create_table_and_start_follower()
        with self.transaction(session=self.session_follow, rollback=True):
            with WiredTigerCursor(self.session_follow, self.uri) as cursor:
                self.assertEqual(cursor[self.key], self.value)
        self.assertTrue(self.follower_has_ingest_metadata(self.layered_uri))

    # Steady-state pickup creates ingest metadata eagerly, while a table picked up at startup
    # remains deferred.
    def test_steady_state_pickup_creates_ingest_eagerly(self):
        self.create_table_and_start_follower()
        self.assertFalse(self.follower_has_ingest_metadata(self.layered_uri))

        self.create_populated_table(self.uri + '_new', timestamp=2)
        self.leader_checkpoint(2)
        self.disagg_advance_checkpoint_and_wait(self.conn_follow)
        self.assertTrue(self.follower_has_ingest_metadata(self.layered_uri + '_new'))
        self.assertEqual(self.lazy_ingest_creation_count(), 0)
