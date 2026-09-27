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

# test_bug038.py
#   WT_CURSOR.remove on a variable-length column-store cursor positioned purely by next()/prev()
#   (never search()) onto a key whose insert-list update chain has nothing live on it must still
#   remove the key, which remains live on the page underneath. The remove must not spuriously
#   return not-found.
class test_bug038(wttest.WiredTigerTestCase):
    uri = 'table:test_bug038'

    directions = [
        ('next', dict(direction='next')),
        ('prev', dict(direction='prev')),
    ]
    scenarios = make_scenarios(directions)

    def create_and_populate(self):
        self.session.create(self.uri, 'key_format=r,value_format=S')
        c = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, 11):
            c[i] = 'value'
        self.session.commit_transaction()
        c.close()

    def evict(self, key):
        """Force the page holding key back out of cache."""
        evict_cursor = self.session.open_cursor(self.uri, None, 'debug=(release_evict)')
        evict_cursor.set_key(key)
        self.assertEqual(evict_cursor.search(), 0)
        evict_cursor.reset()
        evict_cursor.close()

    def leave_aborted_update(self, key):
        """Attach an aborted update to key's own on-page cell, leaving its insert-list entry
        with nothing live on it, the same shape a completed rolled-back transaction leaves
        behind."""
        session = self.conn.open_session()
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        cursor.set_key(key)
        cursor.set_value('phantom')
        cursor.update()
        session.rollback_transaction()
        cursor.close()
        session.close()

    def travel_to_key(self, c, key):
        """
        Move the cursor from its current, appended position onto key using only calls in
        self.direction, so key is reached purely by iteration and never by search(). For 'prev',
        step straight down onto key. For 'next', step all the way down past the beginning of the
        table first, then step back up onto key, so key is still reached solely by next() calls.
        """
        if self.direction == 'prev':
            while c.get_key() != key:
                self.assertEqual(c.prev(), 0)
        else:
            while c.prev() == 0:
                pass
            # The cursor is now past the beginning of the table with no key set;
            # step back onto the first record before iterating up to key.
            self.assertEqual(c.next(), 0)
            while c.get_key() != key:
                self.assertEqual(c.next(), 0)

    def test_remove_after_iteration(self):
        key = 5
        self.create_and_populate()
        self.session.checkpoint()
        self.evict(1)

        self.leave_aborted_update(key)

        # Position the cursor on an appended record past the end of the page, then move onto key
        # purely by iteration. A search that matches an on-page cell sets the var-store
        # on-page-match flag, and that flag would let the underlying check read the page time
        # window and hide the bug; a search that lands on an appended record leaves the flag
        # clear, so key must be reached solely by next()/prev(). The key is still live on the
        # page underneath, so the remove must succeed rather than returning not-found.
        self.session.begin_transaction()
        append_cursor = self.session.open_cursor(self.uri)
        append_cursor[11] = 'value'
        self.session.commit_transaction()
        append_cursor.close()

        c = self.session.open_cursor(self.uri)
        c.set_key(11)
        self.assertEqual(c.search(), 0)
        self.travel_to_key(c, key)

        self.session.begin_transaction('isolation=snapshot')
        self.assertEqual(c.remove(), 0)
        self.session.commit_transaction()
        c.close()

        # Reconciling the page must not trip over disagreeing delete markers for the key.
        self.session.checkpoint()

        verify_cursor = self.session.open_cursor(self.uri)
        verify_cursor.set_key(key)
        self.assertEqual(verify_cursor.search(), wiredtiger.WT_NOTFOUND)
        verify_cursor.close()

if __name__ == '__main__':
    wttest.run()
