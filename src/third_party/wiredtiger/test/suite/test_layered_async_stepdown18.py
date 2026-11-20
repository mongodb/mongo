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
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# A search (or search_near) followed by a walk repositions the constituent the search did not
# match, from the key the search landed on, regardless of which direction the walk then takes. That
# repositioning can land on a prepared cell even when the walk's own direction never reaches it --
# searching a key just short of a prepared one and walking away from it still probes toward the
# prepared key first. A walk resuming after that conflict must not silently treat itself as fully
# positioned again.
@disagg_test_class
class test_layered_async_stepdown18(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),precise_checkpoint=true,' \
        'debug_mode=(disagg_stepdown_prepare=true),'

    directions = [
        ('next', dict(forward=True)),
        ('prev', dict(forward=False)),
    ]

    # search_near additionally drops the position of whichever constituent it did not match,
    # rather than leaving it untouched the way a plain search does.
    entry_points = [
        ('search', dict(use_search_near=False)),
        ('search_near', dict(use_search_near=True)),
    ]

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader",stepdown_write_mirroring=false)'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, directions, entry_points)

    test_name = __qualname__
    uri = f'layered:{test_name}'

    nrows = 40
    blocked_index = nrows // 2

    def keys(self):
        return [f'key{i:04d}' for i in range(self.nrows)]

    def stable_keys(self):
        return [k for i, k in enumerate(self.keys()) if i % 2 == 0]

    def ingest_keys(self):
        return [k for i, k in enumerate(self.keys()) if i % 2 == 1]

    def write_at(self, session, cursor, items, commit_ts):
        session.begin_transaction()
        for k, v in items.items():
            cursor[k] = v
        session.commit_transaction('commit_timestamp=' + self.timestamp_str(commit_ts))

    def test_search_then_walk_over_prepared_key(self):
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1) +
                                 ',stable_timestamp=' + self.timestamp_str(1))
        self.session.create(self.uri, 'key_format=S,value_format=S')

        keys = self.keys()
        blocked_key = keys[self.blocked_index]

        # Search lands here: an ingest key, so the search never touches stable at all, leaving it
        # unpositioned rather than blocked on anything.
        search_key = keys[self.blocked_index - 1]

        cursor = self.session.open_cursor(self.uri, None, None)
        self.write_at(self.session, cursor, {k: 'v-initial' for k in self.stable_keys()}, 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        cursor.close()

        prepare_session = self.conn.open_session()
        prepare_cursor = prepare_session.open_cursor(self.uri, None, None)
        prepare_session.begin_transaction()
        prepare_cursor[blocked_key] = 'v-prepared'
        prepare_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(12))

        self.conn.set_timestamp('step_down_timestamp=' + self.timestamp_str(15))
        writer = self.conn.open_session()
        wcursor = writer.open_cursor(self.uri, None, None)
        self.write_at(writer, wcursor, {k: 'v-initial' for k in self.ingest_keys()}, 20)
        wcursor.close()
        writer.close()

        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(25))

        cursor.set_key(search_key)
        if self.use_search_near:
            self.assertEqual(cursor.search_near(), 0)
        else:
            self.assertEqual(cursor.search(), 0)

        step = cursor.next if self.forward else cursor.prev
        seen = []
        conflicts = 0
        resolved = False
        while True:
            try:
                if step() != 0:
                    break
            except wiredtiger.WiredTigerError as e:
                self.assertIn(
                    wiredtiger.wiredtiger_strerror(wiredtiger.WT_PREPARE_CONFLICT), str(e))
                conflicts += 1
                # Bound the retries so a walk that never makes progress fails as a hang here
                # rather than spinning.
                self.assertLess(conflicts, 10,
                    'the walk never resumed past the prepared key')
                if not resolved:
                    prepare_session.rollback_transaction()
                    resolved = True
                continue
            seen.append((cursor.get_key(), cursor.get_value()))

        self.session.rollback_transaction()
        cursor.close()
        prepare_cursor.close()
        prepare_session.close()

        # Walking toward the prepared key, the conflict is on the walk's own path; walking away
        # from it, the search's own repositioning of the untouched constituent still probes toward
        # the prepared key and conflicts on it, even though the walk itself never reaches that key.
        self.assertEqual(conflicts, 1,
            'the walk must conflict once, before the prepared transaction rolls back')
        if self.forward:
            expected = keys[self.blocked_index:]
        else:
            expected = list(reversed(keys[:self.blocked_index - 1]))
        self.assertEqual(seen, [(key, 'v-initial') for key in expected])

if __name__ == '__main__':
    wttest.run()
