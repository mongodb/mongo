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

import time
import wttest
from wiredtiger import stat

# Regression test for WT-18427: a non-forced WT_SESSION::drop of an already-clean (checkpointed)
# table used to close its dhandle by walking and freeing every page resident in cache,
# synchronously, while holding the schema and dhandle-list write locks. The fix marks a clean
# tree's dhandle dead instead and defers the cache discard to the sweep server, so the drop call
# itself never fully closes the handle.
#
# A clean tree has nothing dirty to flush either way, so comparing the backing file's bytes before
# and after the drop cannot tell the two behaviors apart: neither path writes to the file. The
# property that does tell them apart is whether the discard happens through the sweep server at
# all, not how soon: committing a drop wakes the sweep server so it can reclaim the handle
# promptly, so the deferred discard usually runs within a moment of drop() returning rather than on
# some later scan, and disabling the scan interval doesn't prevent that wake-up. So the statistic
# that counts handles the sweep server closed is the property to poll for, not a stat read
# immediately after drop() returns: with the fix, a clean-tree drop's handle is closed through that
# path and the counter goes up; without it, the drop closes the handle itself before the sweep
# server ever sees it, and the counter does not move.
class test_drop_cache_discard01(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=1G,statistics=(all)'

    value = 'a' * 200

    def populate(self, uri, rows):
        self.session.create(uri, 'key_format=Q,value_format=S')
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(rows):
            cursor[i] = self.value
        cursor.close()

    def get_stat(self, statistic):
        cursor = self.session.open_cursor('statistics:', None, None)
        value = cursor[statistic][2]
        cursor.close()
        return value

    def test_clean_drop_defers_handle_close(self):
        """
        A non-forced drop of an already-clean table must not tear down its file/btree handle as
        part of the drop call: that handle is marked dead and left for the sweep server to close.
        """
        uri = 'table:test_drop_cache_discard01_clean'
        self.populate(uri, 10_000)

        # The checkpoint leaves the tree clean without any wait here: the rows were written with
        # auto-commit and no transaction is open, and a checkpoint advances the oldest id before it
        # reconciles anything, so nothing it writes can be skipped as not yet globally visible and
        # left dirtying the tree behind it.
        self.session.checkpoint()

        dead_close_before = self.get_stat(stat.conn.dh_sweep_dead_close)
        self.session.drop(uri, None)

        deadline = time.time() + 10
        while self.get_stat(stat.conn.dh_sweep_dead_close) == dead_close_before:
            self.assertLess(time.time(), deadline,
                'the sweep server never closed the handle deferred by a clean-tree drop')
            time.sleep(0.1)

    def test_busy_checkpoint_handle_leaves_live_handle_usable(self):
        """
        Dropping a table locks the live handle before any checkpoint handles for the same URI,
        then closes the whole set only once every handle in it is confirmed lockable. If a
        checkpoint handle turns out to be busy, the live handle -- already locked by that point --
        must not have been closed or marked dead: that step is irreversible, so committing it
        before the rest of the set is confirmed available would strand a dead handle behind a drop
        that reports failure. Check this directly: drop must fail EBUSY while a checkpoint cursor
        is open, and the table must still be fully open and usable afterward.
        """
        uri = 'table:test_drop_cache_discard01_busy_checkpoint'
        self.populate(uri, 100)
        self.session.checkpoint('name=wt18427ckpt')

        # Open a checkpoint cursor from a second session so it stays open across the drop attempt.
        session2 = self.conn.open_session()
        ckpt_cursor = session2.open_cursor(uri, None, 'checkpoint=wt18427ckpt')

        self.assertTrue(self.raisesBusy(lambda: self.session.drop(uri, None)),
            'expected drop to fail with EBUSY while a checkpoint cursor is open')

        # Nothing was destroyed: the live table is still fully open and usable.
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(0)
        self.assertEqual(cursor.search(), 0)
        cursor.close()

        ckpt_cursor.close()
        session2.close()

        # With the checkpoint handle free, the drop succeeds.
        self.session.drop(uri, None)

    def test_dirty_drop_still_fails(self):
        """
        A non-forced drop of a table with committed but uncheckpointed content must still fail
        with EBUSY: the fix only changes what happens to an already-clean tree.
        """
        uri = 'table:test_drop_cache_discard01_dirty'
        self.populate(uri, 1)

        self.assertTrue(self.raisesBusy(lambda: self.session.drop(uri, None)),
            'expected drop to fail with EBUSY on a dirty (uncheckpointed) table')

if __name__ == '__main__':
    wttest.run()
