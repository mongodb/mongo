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
# test_layered_cursor27.py
#
# A layered cursor held open across the commit of a read-timestamp transaction must step to the
# neighboring key on the first next()/prev() after the commit. Committing does not reset the
# cursor, and the held key still exists at latest, so iteration must move relative to it.
#
# Read at ts 10 the table is {20:'a20', 30:'a30'} (key 10 is written later). The cursor lands on key
# 20, the transaction commits, then prev() must reach 10 and next() must reach 30.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_cursor27(wttest.WiredTigerTestCase):
    test_name = __qualname__
    fmt = 'key_format=i,value_format=S'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    variants = [
        ('plain', dict(uri=f'table:{test_name}', follower=False)),
        ('leader', dict(uri=f'layered:{test_name}', follower=False)),
        ('follower', dict(uri=f'layered:{test_name}', follower=True)),
    ]
    scenarios = make_scenarios(disagg_storages, variants)

    def conn_config(self):
        return self.extensionsConfig() + ',disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        self.session.create(self.uri, self.fmt)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        if self.follower:
            self.conn_follow = self.wiredtiger_open('follower',
                self.extensionsConfig() + ',create,disaggregated=(role="follower")')
            self.reader = self.conn_follow.open_session('')
            self.reader.create(self.uri, self.fmt)
            self.ignoreStdoutPattern('Picking up the same checkpoint again')
        else:
            self.reader = self.session

    def put(self, session, key, value, ts):
        c = session.open_cursor(self.uri)
        session.begin_transaction()
        c[key] = value
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(ts))
        c.close()

    # Write a value destined for the stable layer: local for plain and leader, via the leader for the
    # follower.
    def put_stable(self, key, value, ts):
        self.put(self.session, key, value, ts)

    # Write a value that shadows the stable layer from the ingest layer: local for plain and leader,
    # on the follower itself otherwise.
    def put_ingest(self, key, value, ts):
        self.put(self.reader, key, value, ts)

    # Seal the stable layer so later writes shadow it from ingest. Plain and leader tables have no
    # stable/ingest split, so there is nothing to do.
    def pick_up_checkpoint(self):
        if not self.follower:
            return
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(20))
        self.session.checkpoint()
        self.disagg_advance_checkpoint(self.conn_follow)

    # Call a bound cursor step method and return (ret, key, value).
    def step(self, op):
        ret = op()
        if ret == wiredtiger.WT_NOTFOUND:
            return (wiredtiger.WT_NOTFOUND, None, None)
        cursor = op.__self__
        return (ret, cursor.get_key(), cursor.get_value())

    # Populate, position a cursor on key 20 at read timestamp 10, commit, and return the cursor.
    def position_across_commit(self):
        self.put_stable(20, 'a20', 10)
        self.put_stable(30, 'a30', 10)
        self.put_stable(10, 'a10', 20)
        self.pick_up_checkpoint()
        self.put_ingest(20, 'b20', 21)

        c = self.reader.open_cursor(self.uri)
        # Key 10 was written at ts 20, so it is invisible to a read at ts 10.
        self.reader.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        self.assertEqual(self.step(c.next), (0, 20, 'a20'))
        self.reader.commit_transaction()
        return c

    def test_prev_after_commit(self):
        c = self.position_across_commit()
        self.assertEqual(self.step(c.prev), (0, 10, 'a10'))
        c.close()

    def test_next_after_commit(self):
        c = self.position_across_commit()
        self.assertEqual(self.step(c.next), (0, 30, 'a30'))
        c.close()
