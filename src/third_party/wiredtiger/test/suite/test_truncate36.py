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
from wtscenario import make_scenarios

# Test rolling back a committed-but-unstable truncate that a checkpoint has already
# persisted, on leaves that were instantiated after that checkpoint and never
# reconciled before rollback_to_stable(). Every truncated key must be restored and
# the tree must still checkpoint cleanly, both while the parent is in memory and
# after it has been reread from disk.
class test_truncate36(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=10MB,statistics=(all)'
    uri = 'table:test_truncate36'
    create_cfg = 'key_format=i,value_format=S,leaf_page_max=32KB'

    value = "abcdefghijklmnopqrstuvwxyz" * 3
    instantiate_keys = [5, 165, 5000, 9719, 10000]
    nrows = 10000

    scenarios = make_scenarios([
        ('prepare', dict(prepare=True)),
        ('no_prepare', dict(prepare=False)),
    ], [
        ('parent_in_memory', dict(reopen_before_rts=False)),
        ('parent_from_disk', dict(reopen_before_rts=True)),
    ])

    def evict_all(self):
        ev = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        while ev.next() == 0:
            pass
        ev.close()

    def check_all_present(self, tag):
        missing = []
        vc = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            vc.set_key(i)
            if vc.search() != 0:
                missing.append(i)
                if len(missing) >= 5:
                    break
            vc.reset()
        vc.close()
        self.assertEqual(missing, [], f'keys NOT FOUND after RTS ({tag}): {missing}')

    def test_truncate_rts_after_checkpoint(self):
        self.session.create(self.uri, self.create_cfg)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        c = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            c[i] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        c.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.checkpoint()
        self.evict_all()

        # Fast-truncate keys 5..end as a committed-but-unstable delete (durable > stable).
        self.session.begin_transaction()
        start = self.session.open_cursor(self.uri)
        start.set_key(5)
        self.session.truncate(None, start, None, None)
        start.close()
        if self.prepare:
            self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(3))
            self.session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(3))
            self.session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(5))
            self.session.commit_transaction()
        else:
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(3))
        stat = self.session.open_cursor('statistics:' + self.uri)
        self.assertGreater(stat[wiredtiger.stat.dsrc.rec_page_delete_fast][2], 0)
        stat.close()

        # Persist the truncate before any truncated leaf is instantiated.
        self.session.checkpoint()
        if self.reopen_before_rts:
            self.reopen_conn()

        # Instantiate a few truncated leaves; nothing reconciles them before RTS.
        inst = self.session.open_cursor(self.uri)
        for k in self.instantiate_keys:
            inst.set_key(k)
            inst.search()
            inst.reset()
        inst.close()

        self.conn.rollback_to_stable()
        self.evict_all()
        self.check_all_present('in memory')

        self.reopen_conn()
        self.check_all_present('after reopen')
