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

import re, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_verify_disagg02.py
#    Verify that duplicate btree IDs among stable files are detected.

@disagg_test_class
class test_verify_disagg02(wttest.WiredTigerTestCase):
    disagg_storages = gen_disagg_storages('test_verify_disagg02', disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = 'disaggregated=(role="leader")'
    conn_config_follower = 'disaggregated=(role="follower")'

    table_cfg = 'key_format=S,value_format=S,block_manager=disagg'
    uri = 'layered:test_verify_disagg02'

    def test_verify_duplicate_btree_ids(self):
        """
        Inject a fake stable file entry with a duplicate btree ID into a follower's local
        metadata, then verify the layered table. The unique btree IDs check in verify should
        detect the duplicate and return an error.
        """
        # Create a layered table on the leader with data, then checkpoint.
        self.session.create(self.uri, self.table_cfg)
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor['key'] = 'value'
        cursor.close()
        self.session.checkpoint()

        # Create a follower and advance it to pick up the checkpoint.
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' +
                                            self.conn_config_follower)
        session_follow = conn_follow.open_session('')
        self.disagg_advance_checkpoint(conn_follow)

        # Read the stable file's config from the follower's metadata to get its btree ID.
        md_cursor = session_follow.open_cursor('metadata:', None, None)
        md_cursor.set_key('file:test_verify_disagg02.wt_stable')
        self.assertEqual(md_cursor.search(), 0)
        victim_config = md_cursor.get_value()
        md_cursor.close()

        # Confirm we got a valid config with an ID.
        self.assertRegex(victim_config, r',id=\d+')

        # Inject a fake entry with the same btree ID into the follower's local metadata.
        raw_cursor = session_follow.open_cursor('file:WiredTiger.wt', None, None)
        raw_cursor.set_key('file:fake_duplicate.wt_stable')
        raw_cursor.set_value(victim_config)
        raw_cursor.insert()
        raw_cursor.close()

        # Verify the layered table. Our check detects the duplicate btree ID.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: session_follow.verify(self.uri), '/WT_ERROR/')
        self.ignoreStderrPatternIfExists('metadata corruption')
        self.ignoreStderrPatternIfExists('stable table verification failed')

        # Remove the fake entry so teardown verification passes.
        raw_cursor = session_follow.open_cursor('file:WiredTiger.wt', None, None)
        raw_cursor.set_key('file:fake_duplicate.wt_stable')
        raw_cursor.remove()
        raw_cursor.close()

        session_follow.close()
        conn_follow.close()
