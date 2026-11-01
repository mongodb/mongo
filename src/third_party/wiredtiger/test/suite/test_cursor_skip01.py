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

import wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# test_cursor_skip01.py
#    The per-session cursor skip statistics count the entries a session steps over
#    while moving its cursors through its own tables, and leave out metadata walks.
class test_cursor_skip01(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(fast)'
    uri = 'table:test_cursor_skip01'

    # Enough removed entries that every scan steps over a large range.
    nentries = 10000

    directions = [
        ('next', dict(forward=True)),
        ('prev', dict(forward=False)),
    ]
    scenarios = make_scenarios(directions)

    def session_stat(self, session, forward):
        which = stat.session.cursor_next_entries_skipped if forward else \
            stat.session.cursor_prev_entries_skipped
        return self.get_stat(which, uri='session', session=session)

    def table_stat(self, uri):
        which = stat.dsrc.cursor_next_skip_total if self.forward else \
            stat.dsrc.cursor_prev_skip_total
        return self.get_stat(which, uri=uri)

    def conn_stat(self):
        which = stat.conn.cursor_next_skip_total if self.forward else \
            stat.conn.cursor_prev_skip_total
        return self.get_stat(which)

    # Walk the whole object in the scenario's direction, returning the records seen.
    def scan(self, session, uri):
        cursor = session.open_cursor(uri)
        step = cursor.next if self.forward else cursor.prev
        found = 0
        while step() == 0:
            found += 1
        cursor.close()
        return found

    # Insert keys and then remove them all, keeping the removes visible only to readers newer
    # than the remove timestamp. The table is never evicted, so the removed entries stay in place.
    def populate_tombstones(self):
        self.session.create(self.uri, 'key_format=i,value_format=S,cache_resident=true')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(5))
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, self.nentries + 1):
            cursor[i] = 'value' + str(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        self.session.begin_transaction()
        for i in range(1, self.nentries + 1):
            cursor.set_key(i)
            self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))
        cursor.close()

    def test_tombstone_scan(self):
        self.populate_tombstones()
        writer_before = self.session_stat(self.session, self.forward)

        reader = self.conn.open_session()
        reader.begin_transaction('read_timestamp=' + self.timestamp_str(30))
        reader_before = self.session_stat(reader, self.forward)
        other_before = self.session_stat(reader, not self.forward)
        table_before = self.table_stat(self.uri)

        self.assertEqual(self.scan(reader, self.uri), 0)

        reader_delta = self.session_stat(reader, self.forward) - reader_before
        table_delta = self.table_stat(self.uri) - table_before
        reader.rollback_transaction()

        # The removed entries stay in the cache and only the reader walks the table, so the count
        # is exact and charged to the reader alone.
        self.assertEqual(reader_delta, self.nentries)
        self.assertEqual(reader_delta, table_delta)
        self.assertEqual(self.session_stat(reader, not self.forward), other_before)
        self.assertEqual(self.session_stat(self.session, self.forward), writer_before)
        reader.close()

    def test_metadata_excluded(self):
        # Dropped objects leave deleted entries in the metadata.
        for i in range(100):
            uri = 'table:test_cursor_skip01_drop' + str(i)
            self.session.create(uri, 'key_format=i,value_format=i')
            self.session.drop(uri)

        reader = self.conn.open_session()
        reader_before = self.session_stat(reader, self.forward)
        conn_before = self.conn_stat()

        self.assertGreater(self.scan(reader, 'metadata:'), 0)

        # The metadata walk skipped the deleted entries, but none are charged to the session.
        self.assertGreater(self.conn_stat(), conn_before)
        self.assertEqual(self.session_stat(reader, self.forward), reader_before)
        reader.close()

if __name__ == '__main__':
    wttest.run()
