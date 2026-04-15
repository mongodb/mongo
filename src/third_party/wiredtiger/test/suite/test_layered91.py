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
from helper_disagg import disagg_test_class
from itertools import permutations, product

# test_layered91.py
#    Test layered cursor iteration.
#
# A follower layered table is more complex than on a leader. In a layered table on a follower
# there are both ingest and stable tables. Assuming we have either a fixed timestamp, or aren't
# using a timestamp, for any key in the stable table, there are two states - either the key exists
# or it doesn't. For the ingest table, there are three states - the key exists, or it doesn't, or
# it has been marked as a tombstone. A tombstone indicates that the key doesn't exist, even if it
# appears in the stable table. So for the combination of ingest and stable table, there are 2x3 = 6
# states. We're going to ignore the state where it doesn't exist in either table as it is
# uninteresting for our testing. We'll label the five remaining states as follows:
#    I  -  key exists in the ingest table only
#    S  -  key exists in the stable table only
#    B  -  key exists in both ingest and stable
#    R  -  key is in stable table and is a tombstone in ingest
#    X  -  key is not in stable table and is a tombstone in ingest
#
# To test iteration thoroughly, we want to have sequences of keys where when we do a 'next' cursor
# call, every transition is possible. For example, if key 1 is state I, we need to have tests that
# have key 2 being each of I,S,B,R,X. etc. Also, opening a cursor leaves it "unpositioned", where
# there is no key, we'll call this special state '0'. We do want to test the transition from 0
# to any state and any state to 0.
#
# The idea of this test is to create sequences of letters from the set above, e.g. IBBXS.
# We don't include 0 in the set, but as if we iterate through five keys in the state IBBXS,
# note that there is an implicit 0 and the front and the back of the string. So the 0 transitions
# will be well tested.
#
# To get complete coverage and more, we'll generate all strings of letters from this set of length
# from 0 to 5 (will raise to 6 following WT-17160). For each string, we'll set of the situation where
# we'll have the exact sequence, and then we'll test iterating forward from beginning to end,
# and backward from end to beginning, checking to make sure we have the expected keys and values.
#
# Using layered tables to create the expected situation requires state transitions, from leader
# to follower, and to pick up checkpoints. These are heavyweight operations, so to save testing
# time, we'll create one table for each situation (naming it by its string), this allows us to
# test rapidly.

def generate_unique_situations(max_len):
    # Create all permutations (up to max_len) of letters where each letter appears 0 to 2 times
    elements = ['I', 'S', 'B', 'R', 'X']
    result = []
    for length in range(max_len + 1):
            for seq in product(elements, repeat=length):
                if all(seq.count(e) <= 2 for e in elements):
                    result.append(seq)
    return sorted(result)

@disagg_test_class
class test_layered91(wttest.WiredTigerTestCase):

    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    def _apply_ops(self, session, uri, sit, inserts, removes, ts):
        '''Insert or remove keys based on their situation letter.'''
        if not any(letter in inserts or letter in removes for letter in sit):
            return
        with self.transaction(session=session, commit_timestamp=ts):
            c = session.open_cursor(uri)
            for key, letter in enumerate(sit, 1):
                if letter in inserts:
                    c[str(key)] = str(key)
                elif letter in removes:
                    c.set_key(str(key))
                    c.remove()
            c.close()

    def _verify_cursor(self, session, uri, sit):
        '''Verify forward/backward iteration and point reads on a layered cursor.'''
        # Keys with state I (ingest only), S (stable only), or B (both) should be visible.
        # R (tombstone over stable) and X (tombstone, no stable entry) should not.
        expect = [str(k) for k, letter in enumerate(sit, 1) if letter in ('I', 'S', 'B')]

        # Forward iteration: cursor.next() should yield exactly the visible keys in order.
        got = []
        c = session.open_cursor(uri)
        for k, v in c:
            self.assertEqual(k, v)
            got.append(k)
        c.close()
        self.assertEqual(expect, got)

        # Backward iteration: cursor.prev() should yield the visible keys in reverse order.
        got_rev = []
        c = session.open_cursor(uri)
        ret = c.prev()
        while ret == 0:
            k, v = c.get_key(), c.get_value()
            self.assertEqual(k, v)
            got_rev.append(k)
            ret = c.prev()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        c.close()
        self.assertEqual(list(reversed(expect)), got_rev)

        # Point reads: cursor.search() must find visible keys and return WT_NOTFOUND for hidden ones.
        c = session.open_cursor(uri)
        for key, letter in enumerate(sit, 1):
            c.set_key(str(key))
            ret = c.search()
            if letter in ('I', 'S', 'B'):
                self.assertEqual(ret, 0)
                self.assertEqual(c.get_value(), str(key))
            else:
                self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        c.close()

    def test_layered91(self):
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + \
                  ',create,' + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')

        # FIXME-WT-17160: Increasing the number of situations results in abort due to cache stuck.
        sits = generate_unique_situations(5)
        ts = 100
        uri_sits = []
        for sit in sits:
            uri = 'table:' + ''.join(sit)
            self.session.create(uri, 'key_format=S,value_format=S,block_manager=disagg,type=layered')
            uri_sits.append((uri, sit))

        # Populate stable keys (S, B, R, X) on the leader.
        for uri, sit in uri_sits:
            self._apply_ops(self.session, uri, sit, inserts='SBRX', removes='', ts=ts)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts + 10)}')
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        # Remove X keys on both leader and follower.
        for session in [self.session, session_follow]:
            for uri, sit in uri_sits:
                self._apply_ops(session, uri, sit, inserts='', removes='X', ts=ts + 20)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(ts + 30)}')
        self.session.checkpoint()

        # On the follower: insert ingest keys (I, B) and tombstone R keys.
        for uri, sit in uri_sits:
            self._apply_ops(session_follow, uri, sit, inserts='IB', removes='R', ts=ts + 40)

        self.disagg_advance_checkpoint(conn_follow)

        # Verify cursor iteration and point reads on the follower.
        for uri, sit in uri_sits:
            self._verify_cursor(session_follow, uri, sit)
