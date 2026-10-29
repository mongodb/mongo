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
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin
from wtscenario import make_scenarios

# A positioned follower cursor observes writes made through another cursor in the same
# transaction, and a committed truncate from the same session after its snapshot was released.
@disagg_test_class
class test_layered_cursor30(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader")'
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    directions = [
        ('forward', dict(forward=True)),
        ('backward', dict(forward=False)),
    ]
    placements = [
        ('ingest_alternate', dict(stable_keys=[100, 640], ingest_keys=[550])),
        ('stable_alternate', dict(stable_keys=[550], ingest_keys=[100, 640])),
    ]
    operations = [(op, dict(operation=op))
                  for op in ('truncate', 'remove', 'update', 'insert', 'modify', 'insert_new')]
    scenarios = make_scenarios(disagg_storages, directions, placements, operations)

    def write(self, session, keys, ts):
        with closing(session.open_cursor(self.uri)) as cursor:
            with self.transaction(session=session, commit_timestamp=ts):
                for key in keys:
                    cursor[key] = 'value'

    def apply_write(self, session):
        if self.operation == 'truncate':
            with closing(session.open_cursor(self.uri)) as lo, \
                closing(session.open_cursor(self.uri)) as hi:
                lo.set_key(100)
                hi.set_key(640)
                session.truncate(None, lo, hi, None)
        else:
            with closing(session.open_cursor(self.uri)) as writer:
                writer.set_key(self.new_key if self.operation == 'insert_new' else 550)
                if self.operation == 'remove':
                    self.assertEqual(writer.remove(), 0)
                elif self.operation == 'modify':
                    self.assertEqual(writer.search(), 0)
                    self.assertEqual(writer.modify([wiredtiger.Modify('V', 0, 1)]), 0)
                else:
                    writer.set_value('new')
                    if self.operation == 'update':
                        self.assertEqual(writer.update(), 0)
                    else:
                        self.assertEqual(writer.insert(), 0)

    def setup_tables(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.write(self.session, self.stable_keys, 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.conn_follow, self.session_follow = self.open_follower()
        self.disagg_wait_for_adoption(self.conn_follow)

        for session in (self.session, self.session_follow):
            self.write(session, self.ingest_keys, 20)

    def test_write_with_positioned_cursor(self):
        self.setup_tables()
        self.new_key = 300 if self.forward else 600
        expected = {100: 'value', 550: 'value', 640: 'value'}
        if self.operation == 'truncate':
            expected.clear()
        elif self.operation == 'remove':
            del expected[550]
        elif self.operation == 'modify':
            expected[550] = 'Value'
        else:
            expected[self.new_key if self.operation == 'insert_new' else 550] = 'new'

        with closing(self.session_follow.open_cursor(self.uri)) as cursor:
            with self.transaction(rollback=True), \
                self.transaction(session=self.session_follow, rollback=True):
                step = cursor.next if self.forward else cursor.prev
                self.assertEqual(step(), 0)
                anchor = 100 if self.forward else 640
                self.assertEqual(cursor.get_key(), anchor)

                for session in (self.session, self.session_follow):
                    self.apply_write(session)

                self.assertEqual(cursor.get_key(), anchor)
                actual = []
                while step() == 0:
                    actual.append((cursor.get_key(), cursor.get_value()))
                expected.pop(anchor, None)
                self.assertEqual(actual, sorted(expected.items(), reverse=not self.forward))

            self.assertEqual([key for key, _ in cursor], [100, 550, 640])

    def test_committed_truncate_with_positioned_cursor(self):
        if self.operation != 'truncate':
            self.skipTest('only truncate invalidates a cursor spanning the commit')
        self.setup_tables()

        with closing(self.session_follow.open_cursor(self.uri)) as cursor:
            step = cursor.next if self.forward else cursor.prev
            self.assertEqual(step(), 0)
            self.assertEqual(cursor.get_key(), 100 if self.forward else 640)

            for session in (self.session, self.session_follow):
                with self.transaction(session=session, commit_timestamp=30):
                    self.apply_write(session)

            self.assertEqual(step(), wiredtiger.WT_NOTFOUND)


# A walk stalled on a prepared ingest key observes writes made by its own transaction during the
# stall, including a truncate hiding the key the stable constituent is parked on.
@disagg_test_class
class test_layered_cursor30_prepare_stall(LayeredFastTruncateConfigMixin,
                                           wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_config = 'disaggregated=(role="leader")'
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    directions = [
        ('forward', dict(forward=True)),
        ('backward', dict(forward=False)),
    ]
    scenarios = make_scenarios(disagg_storages, directions)

    def write(self, session, keys, ts):
        with closing(session.open_cursor(self.uri)) as cursor:
            with self.transaction(session=session, commit_timestamp=ts):
                for key in keys:
                    cursor[key] = 'value'

    def truncate(self, session, lo_key, hi_key):
        with closing(session.open_cursor(self.uri)) as lo, \
            closing(session.open_cursor(self.uri)) as hi:
            lo.set_key(lo_key)
            hi.set_key(hi_key)
            session.truncate(None, lo, hi, None)

    def test_write_while_stalled(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.write(self.session, [100, 400, 640], 10)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()
        self.conn_follow, self.session_follow = self.open_follower()
        self.disagg_wait_for_adoption(self.conn_follow)
        for session in (self.session, self.session_follow):
            self.write(session, [200, 550], 20)

        if self.forward:
            prepared_key, new_key, truncate_range = 300, 700, (600, 640)
            visited, remaining = [100, 200], [(400, 'value'), (550, 'value'), (700, 'new')]
        else:
            prepared_key, new_key, truncate_range = 500, 50, (100, 150)
            visited, remaining = [640, 550], [(400, 'value'), (200, 'value'), (50, 'new')]

        prep_session = self.conn_follow.open_session()
        with closing(prep_session.open_cursor(self.uri)) as prep_cursor:
            prep_session.begin_transaction()
            prep_cursor[prepared_key] = 'prepared'
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(30) +
                                         ',prepared_id=' + self.prepared_id_str(1))

        with closing(self.session_follow.open_cursor(self.uri)) as cursor:
            with self.transaction(session=self.session_follow, rollback=True):
                step = cursor.next if self.forward else cursor.prev
                for key in visited:
                    self.assertEqual(step(), 0)
                    self.assertEqual(cursor.get_key(), key)
                self.assertRaisesException(wiredtiger.WiredTigerError, step,
                                           '/conflict with a prepared update/')

                with closing(self.session_follow.open_cursor(self.uri)) as writer:
                    writer[new_key] = 'new'
                self.truncate(self.session_follow, *truncate_range)
                self.assertRaisesException(wiredtiger.WiredTigerError, step,
                                           '/conflict with a prepared update/')

                prep_session.rollback_transaction()
                actual = []
                while step() == 0:
                    actual.append((cursor.get_key(), cursor.get_value()))
                self.assertEqual(actual, remaining)
