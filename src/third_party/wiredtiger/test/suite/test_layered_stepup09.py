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

# Verify that the stable cursor lifecycle is correct when a cursor survives a
# step-up: the cursor was open on the follower and is reused after the same
# connection becomes the leader.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

_insert_counter = 0

def _op_insert(cursor):
    global _insert_counter
    _insert_counter += 1
    cursor.set_key(f'key_0_{_insert_counter}')
    cursor.set_value('api_val')
    return cursor.insert()

def _op_update(cursor):
    cursor.set_key('key_0')
    cursor.set_value('api_val')
    return cursor.update()

def _op_search(cursor):
    cursor.set_key('key_0')
    return cursor.search()

def _op_search_near(cursor):
    cursor.set_key('key_0')
    return cursor.search_near()

def _op_next(cursor):
    return cursor.next()

def _op_prev(cursor):
    return cursor.prev()

def _op_remove(cursor):
    cursor.set_key('key_0')
    return cursor.remove()

def _op_reserve(cursor):
    cursor.set_key('key_0')
    return cursor.reserve()

def _op_modify(cursor):
    cursor.set_key('key_0')
    return cursor.modify([wiredtiger.Modify('X', 0, 1)])

def _op_largest_key(cursor):
    return cursor.largest_key()

_operations = [
    ('insert',      dict(do_op=_op_insert)),
    ('update',      dict(do_op=_op_update)),
    ('search',      dict(do_op=_op_search)),
    ('search_near', dict(do_op=_op_search_near)),
    ('next',        dict(do_op=_op_next)),
    ('prev',        dict(do_op=_op_prev)),
    ('remove',      dict(do_op=_op_remove)),
    ('reserve',     dict(do_op=_op_reserve)),
    ('modify',      dict(do_op=_op_modify)),
    ('largest_key', dict(do_op=_op_largest_key)),
]

_overwrite = [
    ('overwrite',    dict(overwrite=True)),
    ('no_overwrite', dict(overwrite=False)),
]

@disagg_test_class
class test_layered_stepup09(wttest.WiredTigerTestCase):
    test_name = __qualname__

    uri = f'layered:{test_name}'
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, _operations, _overwrite)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def follower_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")'

    def get_stat(self, session, stat_key):
        stat_cursor = session.open_cursor('statistics:')
        stat_cursor.set_key(stat_key)
        stat_cursor.search()
        val = stat_cursor.get_value()[2]
        stat_cursor.close()
        return val

    def insert_keys(self, session, nkeys, ts):
        cursor = session.open_cursor(self.uri)
        for i in range(nkeys):
            session.begin_transaction()
            cursor[f'key_{i}'] = f'val_{i}'
            session.commit_transaction(f'commit_timestamp={self.timestamp_str(ts)}')
        cursor.close()

    def open_follower(self):
        conn = self.wiredtiger_open('follower', self.follower_config())
        session = conn.open_session('')
        session.create(self.uri, self.table_config)
        cursor_config = None if self.overwrite else 'overwrite=false'
        cursor = session.open_cursor(self.uri, None, cursor_config)
        return conn, session, cursor

    def test_stepup_cursor_had_stable(self):
        """
        The follower had a checkpoint and the stable cursor was open in read-only mode.
        After step-up the same layered cursor object must work correctly as a leader cursor without
        being explicitly closed and reopened.
        """
        self.session.create(self.uri, self.table_config)
        self.insert_keys(self.session, 5, 10)
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()

        conn_follow, session_follow, cursor_follow = self.open_follower()
        self.disagg_advance_checkpoint(conn_follow)

        # Replicate the leader's rows to the follower's ingest.
        # `insert_keys` opens a default (overwrite) cursor so stable stays unopened.
        self.insert_keys(session_follow, 5, 10)
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable), 0)

        # First read opens the stable cursor in read-only mode.
        cursor_follow.set_key('key_0')
        self.assertEqual(cursor_follow.search(), 0)
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable), 1)
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_reopen_stable), 0)

        # Close the leader and step up on the follower.
        cursor_follow.reset()
        self.conn.close('debug=(skip_checkpoint=true)')
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # After step-up, same cursor: stable switches to read-write transparently.
        session_follow.begin_transaction()
        self.do_op(cursor_follow)
        session_follow.rollback_transaction()
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable), 1)
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_reopen_stable), 1)

        cursor_follow.close()
        conn_follow.close()

    def test_stepup_cursor_no_stable(self):
        """
        The follower never picked up a checkpoint, so the stable cursor was never opened.
        After step-up the first cursor use must open stable.
        """
        conn_follow, session_follow, cursor_follow = self.open_follower()

        # Replicate the leader's rows to the follower's ingest.
        # `insert_keys` opens a default (overwrite) cursor so stable stays unopened.
        self.insert_keys(session_follow, 3, 10)
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable), 0)

        # Close the leader and step up on the follower.
        cursor_follow.reset()
        self.conn.close('debug=(skip_checkpoint=true)')
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # After step-up, same cursor: stable opens in read-write mode on first use.
        session_follow.begin_transaction()
        self.do_op(cursor_follow)
        session_follow.rollback_transaction()
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable), 1)
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_reopen_stable), 0)

        cursor_follow.close()
        conn_follow.close()
