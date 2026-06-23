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

# test_layered_cursor24.py
#   A follower cursor positioned on the stable constituent must not reopen the stable table to a
#   newer checkpoint while it is still positioned, even when one is available. A write operation
#   localizes the cursor's key (WT_CURSTD_KEY_INT becomes WT_CURSTD_KEY_EXT) before entering, so the
#   reopen guard must test WT_CURSTD_KEY_SET rather than WT_CURSTD_KEY_INT; otherwise the localized
#   key slips past the guard and the stable cursor is reopened underneath the live position.

import wiredtiger, wttest
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_cursor24(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.extensionsConfig() + ',create,statistics=(all),disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn_follow = self.wiredtiger_open('follower',
            self.extensionsConfig() + ',create,statistics=(all),disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')
        self.session_follow.create(self.uri, 'key_format=i,value_format=S')

    # Write a key on the leader at the given timestamp, checkpoint, and pull it into the follower's
    # stable constituent. Each call publishes a new checkpoint for the follower to pick up.
    def write_stable(self, key, value, ts):
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        cursor.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(ts))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    # The running count of stable btree reopens on the follower.
    def reopen_stable_count(self):
        stat_cursor = self.session_follow.open_cursor('statistics:')
        number = stat_cursor[stat.conn.layered_curs_reopen_stable][2]
        stat_cursor.close()
        return number

    def test_no_reopen_while_positioned_on_stable(self):
        # Seed the key into the stable constituent and position a follower cursor on it.
        self.write_stable(1, 'v1', 1)
        cursor = self.session_follow.open_cursor(self.uri)
        cursor.set_key(1)
        self.assertEqual(cursor.search(), 0)

        # Publish a newer checkpoint (touching a different key), so advancing the stable cursor would
        # now be possible.
        self.write_stable(2, 'v2', 2)

        # A positioned write localizes the key and runs in a later transaction with a read timestamp.
        # The stable cursor is still positioned, so it must not be reopened to pick up the newer
        # checkpoint; without the WT_CURSTD_KEY_SET guard the localized key slips past the check and
        # the stable btree is reopened underneath the live position.
        before = self.reopen_stable_count()
        self.session_follow.begin_transaction('read_timestamp=' + self.timestamp_str(1))
        cursor.set_value('v3')
        self.assertEqual(cursor.update(), 0)
        self.session_follow.commit_transaction('commit_timestamp=' + self.timestamp_str(3))
        self.assertEqual(self.reopen_stable_count(), before)
