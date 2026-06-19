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

# The stable cursor on a follower must not open until the first read after the
# follower has picked up a checkpoint. Writes with default overwrite must never
# open stable; non-overwrite writes and all reads must open it.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

_insert_counter = 0

def _op_insert(cursor):
    # Insert uses a global counter to always generate new keys (so non-overwrite won't fail).
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

_txn_modes = [
    ('rollback', dict(txn_mode='rollback')),
    ('commit',   dict(txn_mode='commit')),
    ('survive',  dict(txn_mode='survive')),
]

@disagg_test_class
class test_layered_follower16(wttest.WiredTigerTestCase):
    test_name = __qualname__

    uri = f'layered:{test_name}'
    table_config = 'key_format=S,value_format=S'
    conn_base_config = ',create,statistics=(all),'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, _operations, _overwrite, _txn_modes)

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

    def end_txn(self, session):
        if self.txn_mode == 'rollback':
            session.rollback_transaction()
        else:
            session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')

    def test_stable_deferred_until_checkpoint(self):
        """
        The stable cursor must not open until the first read after the follower has
        picked up a checkpoint. Writes with overwrite must never open stable.
        """

        self.session.create(self.uri, self.table_config)
        self.insert_keys(self.session, 5, 10)

        conn_follow = self.wiredtiger_open('follower', self.follower_config())
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, self.table_config)

        # Replicate the leader's writes to the follower's ingest.
        self.insert_keys(session_follow, 5, 10)

        # Open the follower cursor.
        cursor_config = 'overwrite=false' if not self.overwrite else None
        cursor_follow = session_follow.open_cursor(self.uri, None, cursor_config)

        # Any operation before a checkpoint must not open stable.
        session_follow.begin_transaction()
        self.do_op(cursor_follow)
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable), 0)

        if self.txn_mode != 'survive':
            self.end_txn(session_follow)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        if self.txn_mode != 'survive':
            session_follow.begin_transaction()

        opens_stable = not (self.overwrite and self.do_op in (_op_insert, _op_update))

        # After the checkpoint arrives, repeat the same operation.
        self.do_op(cursor_follow)
        self.end_txn(session_follow)

        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_open_stable), opens_stable)
        self.assertEqual(self.get_stat(session_follow, wiredtiger.stat.conn.layered_curs_reopen_stable), 0)

        cursor_follow.close()
        conn_follow.close()
