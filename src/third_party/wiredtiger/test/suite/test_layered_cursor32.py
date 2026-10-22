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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wiredtiger import stat
from wtscenario import make_scenarios

# The blind_remove cursor configuration is accepted by WT_SESSION::open_cursor and
# WT_CURSOR::reconfigure on layered tables, and fails with ENOTSUP on any other table.
@disagg_test_class
class test_layered_cursor32(wttest.WiredTigerTestCase):
    table_name = __qualname__

    conn_base_config = ',create,statistics=(all),'
    disagg_storages = gen_disagg_storages(disagg_only=True)
    table_types = [
        ('layered', dict(
            create_config='key_format=S,value_format=S,block_manager=disagg,type=layered',
            supported=True)),
        ('plain', dict(create_config='key_format=S,value_format=S', supported=False)),
    ]
    scenarios = make_scenarios(disagg_storages, table_types)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")'

    def setUp(self):
        super().setUp()
        self.uri = 'table:' + self.table_name
        self.session.create(self.uri, self.create_config)
        self.commit_ts = 0
        self.write({'k1': 'v1'})

    def write(self, items, cursor=None):
        c = cursor if cursor is not None else self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for key, value in items.items():
            c[key] = value
        self.commit_ts += 1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.commit_ts))
        if cursor is None:
            c.close()

    def assert_enotsup(self, expr):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, expr,
            f'/{self.table_name}.*: blind_remove is only supported on layered tables.*' +
            'Operation not supported/')

    def test_open_cursor(self):
        self.session.open_cursor(self.uri, None, 'blind_remove=false').close()
        if self.supported:
            self.session.open_cursor(self.uri, None, 'blind_remove=true,dump=print').close()
        else:
            self.assert_enotsup(
                lambda: self.session.open_cursor(self.uri, None, 'blind_remove=true'))
            self.assert_enotsup(
                lambda: self.session.open_cursor(self.uri, None, 'blind_remove=true,dump=print'))
            return

        # Operations other than remove behave as usual, and removing an existing key succeeds.
        cursor = self.session.open_cursor(self.uri, None, 'blind_remove=true')
        self.write({'k1': 'v1-updated', 'k2': 'v2'}, cursor)
        self.assertEqual(cursor['k1'], 'v1-updated')
        cursor.reset()
        self.assertEqual([key for key, _ in cursor], ['k1', 'k2'])

        self.session.begin_transaction()
        cursor.set_key('k2')
        self.assertEqual(cursor.remove(), 0)
        self.commit_ts += 1
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(self.commit_ts))
        cursor.set_key('k2')
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

    def test_reconfigure(self):
        cursor = self.session.open_cursor(self.uri)
        self.assertEqual(cursor.reconfigure('blind_remove=false'), 0)

        if not self.supported:
            # A failed reconfigure leaves the cursor unchanged.
            cursor.set_key('k1')
            self.assertEqual(cursor.search(), 0)
            self.assert_enotsup(lambda: cursor.reconfigure('blind_remove=true'))
            self.assertEqual(cursor.get_key(), 'k1')
            cursor.close()
            return

        self.assertEqual(cursor.reconfigure('blind_remove=true'), 0)
        self.assertEqual(cursor['k1'], 'v1')
        self.assertEqual(cursor.reconfigure('blind_remove=false'), 0)
        self.assertEqual(cursor.reconfigure('blind_remove=true'), 0)
        cursor.close()

    def test_cursor_cache(self):
        # FIXME-WT-18920: Once blind remove is implemented, check that removing a missing key
        # succeeds after the blind_remove=true open and fails after the default open.

        # Open and close a cursor, leaving it in the session's cursor cache.
        self.session.open_cursor(self.uri).close()

        # Open with the option on, reusing the cached cursor. A layered cursor takes the option from
        # this open. A plain table rejects it, and the cursor still goes back into the cache.
        cache_reuses = self.get_stat(stat.conn.cursor_reopen)
        if self.supported:
            self.session.open_cursor(self.uri, None, 'blind_remove=true').close()
        else:
            self.assert_enotsup(
                lambda: self.session.open_cursor(self.uri, None, 'blind_remove=true'))
        self.assertGreater(self.get_stat(stat.conn.cursor_reopen), cache_reuses)

        # Open with the default configuration, reusing the same cursor. A layered cursor must not
        # keep the earlier option.
        cache_reuses = self.get_stat(stat.conn.cursor_reopen)
        self.session.open_cursor(self.uri).close()
        self.assertGreater(self.get_stat(stat.conn.cursor_reopen), cache_reuses)
