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

# test_layered_tombstone_collision.py
#   A user value byte-identical to the internal tombstone marker (0x14 0x14) must behave like any
#   other value across every cursor operation, never read back as a deletion. Each test runs against
#   every value and in every mode: on the leader, directly on the follower (ingest table), and on
#   the follower after it picks up the value through a checkpoint.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_tombstone_collision(wttest.WiredTigerTestCase):
    test_name = __qualname__
    conn_base_config = ',create,statistics=(all),'
    uri = f'layered:{test_name}'

    values = [
        ('collide', dict(value=b'\x14\x14')),      # exactly the tombstone
        ('control', dict(value=b'\x14\x14\xff')),  # tombstone prefix + a byte
        ('other', dict(value=b'\x14\x14\x14')),    # tombstone prefix + a tombstone byte
    ]
    # Where the value is written and read. 'checkpoint' writes on the leader and reads on the
    # follower after a checkpoint pickup; the others write and read on a single node.
    modes = [
        ('leader', dict(mode='leader')),
        ('follower_direct', dict(mode='follower_direct')),
        ('follower_checkpoint', dict(mode='follower_checkpoint')),
    ]
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, values, modes)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    def setUp(self):
        super().setUp()
        # Reading an escaped value off the stable table logs a warning; that is expected here.
        self.ignoreStdoutPattern('stable table value in the tombstone namespace')
        self.session.create(self.uri, 'key_format=S,value_format=u')
        self.follow_conn = self.wiredtiger_open('follower',
            self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        self.follow = self.follow_conn.open_session('')
        self.follow.create(self.uri, 'key_format=S,value_format=u')

        self.commit_ts = 0
        if self.mode == 'follower_direct':
            self.wsession = self.rsession = self.follow
        elif self.mode == 'follower_checkpoint':
            self.wsession, self.rsession = self.session, self.follow
        else:
            self.wsession = self.rsession = self.session

    # Commit the write transaction. Disaggregated storage requires every transaction to be
    # timestamped; the checkpoint mode also checkpoints at the stable timestamp (see sync).
    def commit(self):
        self.commit_ts += 10
        self.wsession.commit_transaction('commit_timestamp=' + self.timestamp_str(self.commit_ts))

    def write(self, key, value):
        c = self.wsession.open_cursor(self.uri)
        self.wsession.begin_transaction()
        c[key] = value
        self.commit()
        c.close()

    def modify(self, key, mods):
        c = self.wsession.open_cursor(self.uri)
        self.wsession.begin_transaction()
        c.set_key(key)
        self.assertEqual(c.modify(mods), 0)
        self.commit()
        c.close()

    def remove(self, key):
        c = self.wsession.open_cursor(self.uri)
        self.wsession.begin_transaction()
        c.set_key(key)
        self.assertEqual(c.remove(), 0)
        self.commit()
        c.close()

    # Make prior writes visible to the read session. Only the checkpoint mode needs to act.
    def sync(self):
        if self.mode == 'follower_checkpoint':
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.commit_ts))
            self.session.checkpoint()
            self.disagg_advance_checkpoint(self.follow_conn, self.conn)

    def rcursor(self):
        self.sync()
        return self.rsession.open_cursor(self.uri)

    def check(self, key, expected):
        c = self.rcursor()
        c.set_key(key)
        self.assertEqual(c.search(), 0)
        self.assertEqual(c.get_value(), expected)
        c.close()

    def test_search(self):
        self.write('k', self.value)
        self.check('k', self.value)

    def test_next(self):
        self.write('a', self.value)
        self.write('b', self.value)
        c = self.rcursor()
        self.assertEqual(c.next(), 0)
        self.assertEqual((c.get_key(), c.get_value()), ('a', self.value))
        self.assertEqual(c.next(), 0)
        self.assertEqual((c.get_key(), c.get_value()), ('b', self.value))
        c.close()

    def test_prev(self):
        self.write('a', self.value)
        self.write('b', self.value)
        c = self.rcursor()
        self.assertEqual(c.prev(), 0)
        self.assertEqual((c.get_key(), c.get_value()), ('b', self.value))
        self.assertEqual(c.prev(), 0)
        self.assertEqual((c.get_key(), c.get_value()), ('a', self.value))
        c.close()

    def test_search_near(self):
        self.write('k', self.value)
        c = self.rcursor()
        c.set_key('k')
        self.assertEqual(c.search_near(), 0)
        self.assertEqual(c.get_value(), self.value)
        c.close()

    def test_search_near_nonexact(self):
        self.write('a', self.value)
        self.write('c', self.value)
        c = self.rcursor()
        c.set_key('b')
        self.assertNotEqual(c.search_near(), 0)
        self.assertEqual(c.get_value(), self.value)
        c.close()

    def test_update(self):
        self.write('k', b'plain')
        self.write('k', self.value)
        self.check('k', self.value)

    def test_modify_from_value(self):
        # The value is the modify base; append a byte.
        self.write('k', self.value)
        self.modify('k', [wiredtiger.Modify(b'\xaa', len(self.value), 0)])
        self.check('k', self.value + b'\xaa')

    def test_modify_into_value(self):
        # Build the value with a modify by dropping a trailing byte.
        self.write('k', self.value + b'\xaa')
        self.modify('k', [wiredtiger.Modify(b'', len(self.value), 1)])
        self.check('k', self.value)

    def test_modify_out_of_namespace(self):
        # The value is the modify base; replace it with a value outside the namespace.
        self.write('k', self.value)
        self.modify('k', [wiredtiger.Modify(b'ab', 0, len(self.value))])
        self.check('k', b'ab')

    def test_remove(self):
        self.write('k', self.value)
        self.remove('k')
        c = self.rcursor()
        c.set_key('k')
        self.assertEqual(c.search(), wiredtiger.WT_NOTFOUND)
        c.close()

    def test_reinsert_after_remove(self):
        # A delete marker followed by an escaped value on the same update chain.
        self.write('k', self.value)
        self.remove('k')
        self.write('k', self.value)
        self.check('k', self.value)
