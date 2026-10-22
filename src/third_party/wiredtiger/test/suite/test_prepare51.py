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
from wtscenario import make_scenarios

# A cursor that reverses direction after a prepare conflict can return the key it returned before
# the conflict again, or a key its own transaction inserted since. Either way the keys it returns
# afterwards must not trip the key order check.
class test_prepare51(wttest.WiredTigerTestCase):
    uri = 'table:test_prepare51'

    # The prepared key sits past the keys the walk returns, and the walk's own transaction later
    # inserts a key between the last returned key and the prepared one.
    directions = [
        ('forward', dict(forward=True, committed=[1, 2, 3], prepared=5, inserted=4)),
        ('backward', dict(forward=False, committed=[5, 6, 7], prepared=3, inserted=4)),
    ]
    scenarios = make_scenarios(directions)

    def test_reverse_after_conflict(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        with closing(self.session.open_cursor(self.uri)) as cursor:
            for key in self.committed:
                cursor[key] = 'value'

        prep_session = self.conn.open_session()
        with closing(prep_session.open_cursor(self.uri)) as prep_cursor:
            prep_session.begin_transaction()
            prep_cursor[self.prepared] = 'prepared'
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(10))

        step = lambda c: c.next() if self.forward else c.prev()
        step_back = lambda c: c.prev() if self.forward else c.next()

        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        visited = self.committed if self.forward else list(reversed(self.committed))
        for key in visited:
            self.assertEqual(step(cursor), 0)
            self.assertEqual(cursor.get_key(), key)
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: step(cursor),
                                   '/conflict with a prepared update/')

        with closing(self.session.open_cursor(self.uri)) as writer:
            writer[self.inserted] = 'inserted'

        # Reversing returns the inserted key, then every committed key back to the first.
        returned = []
        while step_back(cursor) == 0:
            returned.append(cursor.get_key())
        self.assertEqual(returned, [self.inserted] + list(reversed(visited)))

        cursor.close()
        self.session.rollback_transaction()
        prep_session.rollback_transaction()
