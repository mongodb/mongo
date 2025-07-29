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

import os, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered31.py
#    Extra tests for follower picking up new checkpoints.
@disagg_test_class
class test_layered31(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 500

    # The keys in this test are integer values less than nitems that have been "stringized".
    # Make an array of the keys in sort order so we can verify the results from scanning.
    keys_in_order = sorted([str(k) for k in range(nitems)])

    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    create_session_config = 'key_format=S,value_format=S'

    layered_uris = ["layered:test_layered31a", "layered:test_layered31b"]
    all_uris = list(layered_uris)

    disagg_storages = gen_disagg_storages('test_layered31', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    # Reset a cursor on the follower.  Generally, the test will open a layered: uri,
    # and a reset is a signal have the cursor move to the next checkpoint. This works
    # for layered cursors but not cursors in general.  In the m4 milestone where we don't
    # use a layered cursor, to get similar behavior, we need to reopen the cursor.
    def reset_follow_cursor(self, cursor):
        cursor.reset()
        return cursor

    def put_data(self, value_prefix, low = 0, high = nitems, session = None):
        if session == None:
            session = self.session   # leader by default
        for uri in self.all_uris:
            cursor = session.open_cursor(uri, None, None)
            for i in range(low, high):
                cursor[str(i)] = value_prefix + str(i)
            cursor.close()

    def check_data_follower(self, value_prefix, low = 0, high = nitems, cursors = None, uris = all_uris):
        result_cursors = dict()
        for uri in self.all_uris:
            if cursors:
                cursor = cursors[uri]
            else:
                cursor = self.session_follow.open_cursor(uri)

            for i in range(low, high):
                self.assertEqual(cursor[str(i)], value_prefix + str(i))
            result_cursors[uri] = cursor
        return result_cursors

    # Scan data from low to high expecting to see all the keys and values using the given prefix.
    #
    # This function is sometimes called doing partial scans, and later, after a state change,
    # continuing using the same cursor.  We are promised that cursor iteration results aren't
    # affected by other transactions. Extending this reasoning to state changes, like picking up
    # checkpoints and stepping up to leader, cursors should similarly be unaffected by state
    # changes happening concurrently to the lifetime of the cursor.
    def scan_data_follower(self, value_prefix, low = 0, high = nitems, cursors = None, uris = all_uris):
        result_cursors = dict()
        if value_prefix == 'eee':
            self.session_follow.breakpoint()
        for uri in self.all_uris:
            if cursors:
                cursor = cursors[uri]
            else:
                cursor = self.session_follow.open_cursor(uri)

            found = 0
            for i in range(low, high):
                ret = cursor.next()
                if ret == wiredtiger.WT_NOTFOUND:
                    break
                self.assertEqual(ret, 0)

                expected_key = self.keys_in_order[i]
                self.assertEqual(cursor.get_key(), expected_key)
                self.assertEqual(cursor.get_value(), value_prefix + expected_key)
                found += 1
            result_cursors[uri] = cursor
            self.assertEqual(found, high - low)
        return result_cursors

    def close_cursors(self, cursors):
        for uri in self.all_uris:
            cursor = cursors[uri]
            cursor.close()

    def reset_cursors(self, cursors):
        for uri in self.all_uris:
            cursor = cursors[uri]
            cursors[uri] = self.reset_follow_cursor(cursor)

    # Test more than one table.
    def test_layered31(self):
        # Create all tables in the leader
        for uri in self.all_uris:
            cfg = self.create_session_config
            if not uri.startswith('layered'):
                cfg += ',block_manager=disagg,log=(enabled=false)'
            self.session.create(uri, cfg)

        # Create the follower
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + ',create,' + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')

        self.session_follow = session_follow   # Useful for convenience functions

        #
        # Setup: Check data in the follower normally.
        #

        # Put data to all tables, version 0
        value_prefix0 = '---'
        self.put_data(value_prefix0)

        self.session.checkpoint()

        # Check data in the follower
        self.disagg_advance_checkpoint(conn_follow)
        cursors = self.check_data_follower(value_prefix0)
        self.close_cursors(cursors)

        #
        # Part 1: Check data in the follower, but keep the cursors open.
        #

        # Put data to all tables, version 1
        value_prefix1 = 'aaa'
        self.put_data(value_prefix1)

        self.session.checkpoint()

        # Check data in the follower
        self.disagg_advance_checkpoint(conn_follow)
        follower_cursors = self.check_data_follower(value_prefix1)
        # keep the follower cursors

        #
        # Part 2: Close and reopen the cursor after picking up a checkpoint.
        #

        # Put data to all tables, version 2
        value_prefix2 = 'bbb'
        self.put_data(value_prefix2)

        self.session.checkpoint()

        # Check data in the follower
        self.disagg_advance_checkpoint(conn_follow)
        self.close_cursors(follower_cursors)
        follower_cursors = self.check_data_follower(value_prefix2)
        # keep the follower cursors

        #
        # Part 3: Reset the cursor after picking up a checkpoint.
        #

        # Put data to all tables, version 3
        value_prefix3 = 'ccc'
        self.put_data(value_prefix3)

        self.session.checkpoint()

        # Check data in the follower after a reset
        self.disagg_advance_checkpoint(conn_follow)
        self.reset_cursors(follower_cursors)
        follower_cursors = self.check_data_follower(value_prefix3, cursors=follower_cursors)

        #
        # Part 4: Check that a follower's open cursor's position
        # does not change after picking up a checkpoint.
        #

        # In scanning, do a first batch, reading half the items.
        first_read = self.nitems // 2

        # On the follower, scan and check the first half, leaving cursors open
        self.reset_cursors(follower_cursors)
        follower_cursors = self.scan_data_follower(value_prefix3, 0, first_read, cursors=follower_cursors)

        # Make a change on the leader, and propogate to the follower.
        value_prefix4 = 'ddd'
        self.put_data(value_prefix4)

        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        # Check the continuation of each scan in the follower. Pure cursor scans should be insulated from state changes.
        #
        # Note that we are checking with layered URIs only.
        # Non-layered URIs in this test have a hack (in reset_follow_cursors)
        # that reopens those cursors, thus losing their position.
        follower_cursors = self.scan_data_follower(value_prefix3, first_read, self.nitems, cursors=follower_cursors, uris=self.layered_uris)

        # Now check that after closing, we get the new value
        self.close_cursors(follower_cursors)
        follower_cursors = self.scan_data_follower(value_prefix4, uris=self.layered_uris)

        #
        # Part 5: Check that a follower's open cursor's position
        # does not change after stepping up to leader.
        #
        self.reset_cursors(follower_cursors)
        follower_cursors = self.scan_data_follower(value_prefix4, 0, first_read, cursors=follower_cursors)

        # Make a change on the leader, and propogate to the follower.
        value_prefix5 = 'eee'
        self.put_data(value_prefix5)

        # Advance the checkpoint, but leave cursors open
        self.session.checkpoint()
        self.disagg_advance_checkpoint(conn_follow)

        # Step up, leaving cursors open
        # At this point, we have two connections, our old leader and the follower that is
        # becoming the new leader. Close the old leader first so there's no confusion within this test.
        self.conn.close()
        conn_follow.reconfigure('disaggregated=(role="leader")')

        # Check the continuation of each scan.  Again, we are checking with layered URIs only.
        cursors = self.scan_data_follower(value_prefix4, first_read, self.nitems, cursors=follower_cursors, uris=self.layered_uris)

        # Now check that after closing, we get the new value
        self.close_cursors(cursors)
        cursors = self.scan_data_follower(value_prefix5, uris=self.layered_uris)
        self.close_cursors(cursors)

        #
        # Part 6: Check that the new leader's open cursor's position
        # does not change after stepping back down to follower.
        #
        # FIXME-WT-14545: enable this test when stepping down is debugged.
        if False:
            # Read the first half.
            cursors = self.scan_data_follower(value_prefix5, 0, first_read)

            # Make a change on this new leader
            value_prefix6 = 'fff'
            self.put_data(value_prefix6, session=session_follow)

            # Step down.
            conn_follow.reconfigure('disaggregated=(role="follower",checkpoint_meta=)')

            # Read a couple items to make sure the cursor has been insulated.
            cursors = self.scan_data_follower(value_prefix5, first_read, first_read + 2)

            # Make sure that writing to these layered cursors is now disallowed.
            for uri in self.layered_uris:
                cursor = cursors[uri]
                cursor.set_key('a')
                cursor.set_key('b')
                msg = '/conflict between concurrent operations/'
                self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.insert(), msg1)

            self.close_cursors(cursors)

        session_follow.close()
        conn_follow.close()
