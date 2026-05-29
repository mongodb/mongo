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

import wiredtiger
import wttest
from wiredtiger import stat
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered_cursor20.py
# Follower tests of cached cursors
@disagg_test_class
class test_layered_cursor20(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'
    conn_config_follower = 'disaggregated=(role="follower")'

    nuri = 10
    uri = "layered:test_layered_cursor20"

    disagg_storages = gen_disagg_storages('test_layered_cursor20', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    _ts = 0
    def next_ts(self):
        self._ts += 1
        return self._ts

    def show_cursor_create_stats(self, session, label):
        scursor = session.open_cursor('statistics:', None, None)
        cursor_create = scursor[stat.conn.cursor_create][2]
        self.pr(f'{label}: cursor_create: {cursor_create}')
        scursor.close()

    def check_cursor_create_stats(self, session, limit):
        scursor = session.open_cursor('statistics:', None, None)
        cursor_create = scursor[stat.conn.cursor_create][2]
        self.assertLess(cursor_create, limit)
        scursor.close()

    def test_standby_open_cursor(self):
        # Make a follower
        self.conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() +
                                ',create,statistics=(all),' + self.conn_config_follower)
        self.session_follow = self.conn_follow.open_session('')

        for i in range(0, self.nuri):
            self.session.create(f"{self.uri}_{i}", 'key_format=S,value_format=S')
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(self.next_ts())}')
        self.session.checkpoint()

        for bigloop_count in range(0, 10):
            # Every time through the big loop, we create stuff on the leader, checkpoint it,
            # advance the checkpoint and then read the stuff (several times) on the follower.

            ts = self.next_ts()
            self.pr(f'committing at {ts}')
            with self.transaction(commit_timestamp = ts):
                # The first two tables have content on every checkpoint.
                # The other tables are written once and never again.
                for i in range(0, self.nuri):
                    if i >= 2 and bigloop_count > 0:
                        continue
                    c = self.session.open_cursor(f'{self.uri}_{i}')
                    c['a'] = str(bigloop_count)
                    c.close()

            # Checkpoint at this timestamp, and pick it up on follower
            self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts)}')
            self.session.checkpoint()
            self.disagg_advance_checkpoint(self.conn_follow)

            # On the follower, we are read-heavy.
            # Five times through a loop that visits all the URIs.
            for follower_count in range(0, 5):
                for i in range(0, self.nuri):
                    c = self.session_follow.open_cursor(f'{self.uri}_{i}')
                    if i < 2:
                        self.assertEqual(c['a'], str(bigloop_count))
                    else:
                        self.assertEqual(c['a'], '0')
                    c.close()

            self.show_cursor_create_stats(self.session, 'leader')
            self.show_cursor_create_stats(self.session_follow, 'follower')

        # Every time we do a cursor create, it's a situation where we
        # might have been able to use a cached cursor instead. So numbers
        # that are "too high" indicate we are caching cursors.
        #
        # The numbers below are determined empirically, after running with
        # temporary instrumentation showing what cursors are cached and created,
        # and when. These numbers tend to be higher than we might want because
        # metadata cursors are not cached, and these make up the bulk of cursor opens.
        # FIXME-WT-17299 By getting metadata caching, we should be able to calculate these
        # numbers more closely.
        self.check_cursor_create_stats(self.session, 75)
        self.check_cursor_create_stats(self.session_follow, 325)
