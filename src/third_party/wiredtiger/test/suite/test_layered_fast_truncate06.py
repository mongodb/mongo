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

# test_layered_fast_truncate06.py
#   Regression test for WT-17267. Verify on a layered URI forces a close/reopen of the
#   layered dhandle. The follower's in-memory truncate list must survive that cycle;
#   before the fix it was discarded, causing truncated rows to reappear and the truncate
#   to be torn down.

import wttest, wiredtiger
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_fast_truncate06(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader"),'
    nrows = 100

    uris = [
        ('layered', dict(uri='layered:test_layered_fast_truncate06')),
        ('table', dict(uri='table:test_layered_fast_truncate06')),
    ]

    disagg_storages = gen_disagg_storages(
        'test_layered_fast_truncate06', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, uris)

    def setUp(self):
        if wiredtiger.disagg_fast_truncate_build() == 0:
            self.skipTest("fast truncate support is not enabled")
        super().setUp()

    def create_config(self):
        cfg = 'key_format=i,value_format=S'
        # Disagg table: URIs need the layered block-manager/type hints on create.
        if self.uri.startswith('table:'):
            cfg += ',block_manager=disagg,type=layered'
        return cfg

    def visible_keys(self):
        c = self.session.open_cursor(self.uri)
        keys = []
        while c.next() == 0:
            keys.append(c.get_key())
        c.close()
        return keys

    def setup_follower(self):
        # Create the table on the leader, load nrows, checkpoint, then reopen the
        # connection as a follower picking up that checkpoint.
        self.session.create(self.uri, self.create_config())

        cursor = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            cursor[i] = 'v'
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i))
        cursor.close()
        self.session.checkpoint()

        follower_config = ('disaggregated=(role="follower",'
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")')
        self.reopen_conn(config=follower_config)

    def follower_truncate(self, start, stop):
        c_start = self.session.open_cursor(self.uri)
        c_start.set_key(start)
        c_stop = self.session.open_cursor(self.uri)
        c_stop.set_key(stop)
        self.session.begin_transaction()
        self.session.truncate(None, c_start, c_stop, None)
        self.session.commit_transaction()
        c_start.close()
        c_stop.close()

    def test_verify_preserves_follower_truncate(self):
        self.setup_follower()
        self.follower_truncate(30, 60)

        expected = [i for i in range(1, self.nrows + 1) if i < 30 or i > 60]

        # Before verify: a scan does not return the truncated rows.
        self.assertEqual(self.visible_keys(), expected)

        # Verify the layered URI. This triggers a close + reopen of the dhandle.
        self.session.verify(self.uri)

        # After verify: a scan must still not return the truncated rows.
        self.assertEqual(self.visible_keys(), expected)
