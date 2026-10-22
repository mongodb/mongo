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


from contextlib import closing
import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# A follower walk that outlives its snapshot conflicts with a prepare made after that snapshot.
@disagg_test_class
class test_layered_cursor31(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'
    uri = 'layered:test_layered_cursor31'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    directions = [
        ('forward', dict(forward=True, visited=[100, 300, 340])),
        ('backward', dict(forward=False, visited=[600, 400, 360])),
    ]
    scenarios = make_scenarios(disagg_storages, directions)

    def write(self, session, keys, ts):
        with closing(session.open_cursor(self.uri)) as cursor:
            with self.transaction(session=session, commit_timestamp=ts):
                for key in keys:
                    cursor[key] = 'value'

    def test_prepare_after_snapshot(self):
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() +
                                           ',create,statistics=(all),disaggregated=(role="follower")')
        session_follow = conn_follow.open_session()
        for session in (self.session, session_follow):
            session.create(self.uri, 'key_format=i,value_format=S')
        self.write(self.session, [300, 340, 350, 360, 400], 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)
        self.write(session_follow, [100, 600], 20)

        with closing(session_follow.open_cursor(self.uri)) as cursor:
            step = cursor.next if self.forward else cursor.prev
            session_follow.begin_transaction()
            for key in self.visited:
                self.assertEqual(step(), 0)
                self.assertEqual(cursor.get_key(), key)

            prep_session = conn_follow.open_session()
            prep_session.begin_transaction()
            with closing(prep_session.open_cursor(self.uri)) as prep_cursor:
                prep_cursor.set_key(350)
                self.assertEqual(prep_cursor.remove(), 0)
            prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(30) +
                                             ',prepared_id=' + self.prepared_id_str(1))

            # A read-only commit leaves the cursor positioned, and the next step reads with a new
            # snapshot that includes the prepare.
            session_follow.commit_transaction()
            self.assertRaisesException(wiredtiger.WiredTigerError, step,
                                       '/conflict with a prepared update/')
            prep_session.rollback_transaction()
            self.assertEqual(step(), 0)
            self.assertEqual(cursor.get_key(), 350)
