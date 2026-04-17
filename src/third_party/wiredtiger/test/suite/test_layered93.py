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

# test_layered93.py
#   Test that cursor operations succeed (or fail) on a follower for keys that exist
#   only in the stable table (i.e. written by the leader and checkpointed).

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# ---------------------------------------------------------------------------
# Per-operation wrappers.  Each takes (cursor, key) and returns the raw
# cursor API return value so the test body can assert on it uniformly.
# ---------------------------------------------------------------------------

def _op_reserve(cursor, key):
    cursor.set_key(key)
    return cursor.reserve()

def _op_search(cursor, key):
    cursor.set_key(key)
    return cursor.search()

def _op_search_near(cursor, key):
    cursor.set_key(key)
    return cursor.search_near()

def _op_update(cursor, key):
    cursor.set_key(key)
    cursor.set_value('updated_value')
    return cursor.update()

def _op_remove(cursor, key):
    cursor.set_key(key)
    return cursor.remove()

def _op_modify(cursor, key):
    cursor.set_key(key)
    # Replace the first character of 'value<key>' with 'X'.
    return cursor.modify([wiredtiger.Modify('X', 0, 1)])

_operations = [
    ('reserve',     dict(do_op=_op_reserve)),
    ('search',      dict(do_op=_op_search)),
    ('search_near', dict(do_op=_op_search_near)),
    ('update',      dict(do_op=_op_update)),
    ('remove',      dict(do_op=_op_remove)),
    ('modify',      dict(do_op=_op_modify)),
]

@disagg_test_class
class test_layered93(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'

    uri = 'layered:test_layered93'

    disagg_storages = gen_disagg_storages('test_layered93', disagg_only=True)
    scenarios = make_scenarios(disagg_storages, _operations)

    conn_follow = None
    session_follow = None

    def create_follower(self):
        self.conn_follow = self.wiredtiger_open(
            'follower',
            self.extensionsConfig() + ',create,disaggregated=(role="follower")')
        self.session_follow = self.conn_follow.open_session('')

    def insert_keys(self, nkeys):
        cursor = self.session.open_cursor(self.uri)
        for i in range(1, nkeys + 1):
            self.session.begin_transaction()
            cursor[i] = 'value' + str(i)
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(i))
        cursor.close()

    def test_follower_ops_on_stable_table(self):
        """
        Run self.do_op for a key on the follower. Keys 1-10 exist only in the stable table.
        """
        self.session.create(self.uri, 'key_format=i,value_format=S')

        nkeys = 10
        self.insert_keys(nkeys)

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(nkeys))
        self.session.checkpoint()

        self.create_follower()
        self.disagg_advance_checkpoint(self.conn_follow)
        cursor_follow = self.session_follow.open_cursor(self.uri)

        key = 5
        self.session_follow.begin_transaction()
        self.assertEqual(self.do_op(cursor_follow, key), 0)
        self.session_follow.rollback_transaction()
        cursor_follow.close()
