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

import wttest
from wtscenario import make_scenarios

# Test that rolling back a committed-but-unstable truncate restores every truncated
# key. A checkpoint persists the truncate, whose durable timestamp is ahead of
# stable, as per-key stop tombstones, and correctness then relies on
# rollback_to_stable() undoing it. Run under the disagg hook, this covers a leader.
class test_truncate32(wttest.WiredTigerTestCase):

    # The constants are tuned to the leaf layout: leaf_page_max=32KB with this value
    # gives ~56 leaves, the first holding keys 1..165 and the last 9719..10000. The
    # truncate starts at key 5 so the first leaf is partially truncated, and the
    # instantiate keys cover the first, a middle and the last truncated leaf.
    # Instantiating only those reproduces the failure; reading the whole range shifts
    # cache pressure and hides it.
    conn_config = 'cache_size=10MB,statistics=(all)'
    uri = 'table:test_truncate32'
    create_cfg = 'key_format=i,value_format=S,leaf_page_max=32KB'

    value = "abcdefghijklmnopqrstuvwxyz" * 3
    instantiate_keys = [5, 165, 5000, 9719, 10000]

    scenarios = make_scenarios([
        ('prepare', dict(prepare=True)),
        ('no_prepare', dict(prepare=False)),
    ])

    nrows = 10000

    def test_truncate_rts(self):
        self.session.create(self.uri, self.create_cfg)
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))

        # Insert the base data and write it out.
        c = self.session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            self.session.begin_transaction()
            c[i] = self.value
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))
        c.close()
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.checkpoint()

        # Truncate keys 5..end as a committed-but-unstable delete (durable >
        # stable); RTS must undo it.
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

        # Instantiate a few truncated leaves into per-key tombstones -- the chain
        # the checkpoint persists with the unstable stop.
        inst = self.session.open_cursor(self.uri)
        for k in self.instantiate_keys:
            inst.set_key(k)
            inst.search()
            inst.reset()
        inst.close()

        # Advance stable below the durable timestamp of the truncate (prepare only).
        if self.prepare:
            self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(4) +
                                    ',stable_timestamp=' + self.timestamp_str(4))

        # Checkpoint: persist the unstable stop and mark the values durable.
        self.session.checkpoint()

        # RTS aborts the in-memory tombstones, leaving the durable values.
        self.conn.rollback_to_stable()

        # Force every leaf to reconcile and evict.
        ev = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        while ev.next() == 0:
            pass
        ev.close()

        # A fresh read re-instantiates each leaf; every truncated key must be back.
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
        self.assertEqual(missing, [],
                         f'keys NOT FOUND after RTS: {missing}')
