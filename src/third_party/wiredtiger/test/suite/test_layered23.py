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

import wttest, wiredtiger
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wiredtiger import stat

# test_layered23.py
#    Test the basic ability to insert on a follower.

# The oplog simulates the MongoDB oplog and does a bit more.
# Basic operations:
# - Append a bunch of k,v pairs to the oplog. For convenience, this class chooses
#   the keys (string-ized timestamp) and initial values (the same string-ized timestamp).
#   The values may later be changed by update operations.  Timestamp advances on each append.
#
# - Add some update of k,v pairs to the oplog.  From keys already available, some are
#   updated to new values.  Timestamp advances on each update.
#
# - Apply some entries of the oplog to a WT session.  It is assumed that
#   the URIs needed are already created.
#
# - Check the current state of the oplog (up to some position) against tables
#   accessed by a WT session. Both point reads and a read scan are done.
class Oplog(object):
    def __init__(self, key_size=-1, value_size=-1):
        self._timestamp = 0       # last (1-based) timestamp used
        self._entries = []        # list of (table,k,v) triples.
        self._uris = []           # list of (uri,entlist) pairs.  Each entlist is an ordered list of
                                  # offsets into _entries that apply to this table.
        self._lookup = dict()     # lookup keyed by (table,k) pairs, gives a list of (ts,v) pairs.
        self._tombstone_value = 'tombstone'

        # For debugging - when _use_timestamps is false, don't actually
        # use the internally generated timestamps with WT calls.
        self._use_timestamps = True
        self.key_size = key_size
        self.value_size = value_size

    # Generate the key from an int, by default a simple string.
    # The key size can be modified to make this longer.
    def gen_key(self, i):
        result = str(i)
        if self.key_size > 0 and self.key_size > len(result):
            result += '.' * (self.key_size - len(result))
        return result

    # Generate the key integer, the reverse of gen_key.
    def decode_key(self, s):
        if self.key_size <= 0:
            return int(s)
        if '.' in s:
            end = s.index('.')
        else:
            end = len(s)
        result = int(s[:end])
        if self.key_size > end:
            if s[end:] != '.' * (self.key_size - end):
                raise Exception(f'Oplog key returned: {s}, unexpected format for key_size={self.key_size}')
        elif end != len(s):
            raise Exception(f'Oplog key returned: {s}, unexpected format for key_size={self.key_size}')
        return result

    # Generate the value from an int, by default a simple string.
    # Note: this can be overridden, as long as decode_value is as well
    def gen_value(self, i):
        result = str(i)
        if self.value_size > 0 and self.value_size > len(result):
            result += '.' * (self.value_size - len(result))
        return result

    def add_uri(self, uri):
        self._uris.append((uri,[]))
        return len(self._uris)     # URI table numbers are 1-based.

    def last_timestamp(self):
        return self._timestamp

    # Get the entlist for a table. The entlist is a list of entries
    # in the oplog (integer 0-based positions) that represent
    # all the operations on a table.
    def _get_entlist(self, table):
        if table <= 0 or table > len(self._uris):
            raise Exception('oplog.append: bad table id')
        return self._uris[table - 1][1]

    # Append a single entry to the oplog, incrementing the internal timestamp.
    # Also update the entlist that is attached to the _uris table, it
    # indicates which oplog entries have activity for a given table -
    # useful for updates.  Also update the _lookup, which gives them
    # current value and history for each key.
    #
    def _append_single(self, table, entlist, k, v):
        self._timestamp += 1
        pos = len(self._entries)
        self._entries.append((table,k,v))
        entlist.append(pos)
        look_key = (table,k)
        look_value = (self._timestamp,v)
        if not look_key in self._lookup:
            # First entry - make a list of ts,value pairs
            self._lookup[look_key] = [look_value]
        else:
            self._lookup[look_key].append(look_value)

    # Get the current value of a key, ignoring timestamps
    def _current_value(self, table, k):
        pairs = self._lookup[(table,k)]
        # the last pair will be the most recent oplog entry
        (_,v) = pairs[-1]
        return v

    # Add inserts to the oplog, return the first entry position
    def insert(self, table, count, start_value=None):
        first_pos = len(self._entries)
        entlist = self._get_entlist(table)

        # Use the timestamp as the key by default, that guarantees these
        # will be inserts, as they've never been seen before.
        ts = self._timestamp + 1
        if start_value == None:
            start_value = ts
        for i in range(start_value, start_value + count):
            self._append_single(table, entlist, i, i)
        return first_pos

    # Update some entries in the oplog for the table
    def update(self, table, count):
        first_pos = len(self._entries)
        entlist = self._get_entlist(table)

        if len(entlist) == 0:
            # If oplog has no entries for this table,
            # silently succeed
            return
        for i in range(0, count):
            # entindex is the entry we'll update
            entindex = entlist[i]
            (gottable,k,v) = self._entries[entindex]
            if gottable != table:
                raise Exception(f'oplog internal error: intindex for {table} ' + \
                                f'references oplog entry {entindex}, which is not for this table')
            self._append_single(table, entlist, k, v+k)
        return first_pos

    # Remove some entries in the oplog for the table
    def remove(self, table, count):
        first_pos = len(self._entries)
        entlist = self._get_entlist(table)

        if len(entlist) == 0:
            # If oplog has no entries for this table, silently succeed
            return
        for i in range(0, count):
            # entindex is the entry we'll update
            entindex = entlist[i]
            (gottable,k,_) = self._entries[entindex]
            if gottable != table:
                raise Exception(f'oplog internal error: intindex for {table} ' + \
                                f'references oplog entry {entindex}, which is not for this table')
            self._append_single(table, entlist, k, self._tombstone_value)
        return first_pos

    # Apply the oplog entries starting at the position to the session
    def apply(self, testcase, session, pos, count):
        # Keep a cache of open cursors
        cursors = [None] * len(self._entries)
        while count > 0:
            (table, k, v) = self._entries[pos]
            ts = pos + 1
            cursor = cursors[table - 1]
            if not cursor:
                uri = self._uris[table - 1][0]
                cursor = session.open_cursor(uri)
                cursors[table - 1] = cursor
            session.begin_transaction()
            if v == self._tombstone_value:
                cursor.set_key(self.gen_key(k))
                cursor.remove()
            else:
                cursor[self.gen_key(k)] = self.gen_value(v)
            if self._use_timestamps:
                session.commit_transaction(f'commit_timestamp={testcase.timestamp_str(ts)}')
            else:
                session.commit_transaction()
            pos += 1
            count -= 1
        for cursor in cursors:
            if cursor:
                cursor.close()

    def check(self, testcase, session, pos, count):
        # Keep a cache of open cursors
        cursors = [None] * len(self._entries)

        pos_limit = pos + count
        # Walk through oplog entries doing point-reads at timestamps
        while count > 0:
            (table, k, v) = self._entries[pos]
            ts = pos + 1
            cursor = cursors[table - 1]
            if not cursor:
                uri = self._uris[table - 1][0]
                cursor = session.open_cursor(uri)
                cursors[table - 1] = cursor
            if self._use_timestamps:
                expected_value_int = v
                session.begin_transaction(f'read_timestamp={testcase.timestamp_str(ts)}')
            else:
                expected_value_int = self._current_value(table, k)
                session.begin_transaction()
            if expected_value_int == self._tombstone_value:
                cursor.set_key(self.gen_key(k))
                testcase.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
            else:
                actual_key = self.gen_key(k)
                expected_value = self.gen_value(expected_value_int)
                result_value = cursor[actual_key]
                if (result_value != expected_value):
                    testcase.pr(f'point-read of {actual_key} at ts={ts} gives {result_value}, expected {expected_value}')
                    testcase.assertEqual(result_value, expected_value)
            session.rollback_transaction()
            pos += 1
            count -= 1
        for cursor in cursors:
            if cursor:
                cursor.close()

        # Do a cursor scan, compare against most recent.
        for uri, entlist in self._uris:
            # Set up the values we think should be present for this table
            values = dict()        # key -> value for most recent value
            prev = -1
            for entindex in entlist:
                if entindex < prev:
                    raise Exception(f'oplog: intindex for {table} is out of order')
                if entindex >= pos_limit:
                    break
                (_,k,v) = self._entries[entindex]
                values[k] = v    # overwrites in time order, so we end up with most recent

            # Walk the cursor and check
            cursor = session.open_cursor(uri)
            for k,v in cursor:
                kint = self.decode_key(k)
                if not kint in values:
                    testcase.pr(f'FAILURE got unexpected key {kint}, value {v} from cursor')
                elif v != self.gen_value(values[kint]):
                    testcase.pr(f'FAILURE at key {kint}, got value {v} want {values[kint]}')
                testcase.assertEqual(v, self.gen_value(values[kint]))
                del values[kint]
            cursor.close()

    def __str__(self):
        return 'Oplog:' + \
            f' timestamp={self._timestamp}, use_timestamp={self._use_timestamps}' + \
            f' entries - list of (table,k,v)={self._entries},' + \
            f' uris - list of (uri, entlist)={self._uris},' + \
            f' lookup - (table,k) -> list of (ts,value)={self._lookup}'

@disagg_test_class
class test_layered23(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                 + 'disaggregated=(page_log=palm),'
    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    scenarios = gen_disagg_storages('test_layered23', disagg_only = True)

    uri = "layered:test_layered23"

    # Make sure the stats agree that the leader has done each checkpoint.
    def check_checkpoint(self, expected):
        stat_cur = self.session.open_cursor('statistics:', None, None)
        self.assertEqual(stat_cur[stat.conn.checkpoints_total_succeed][2], expected)
        stat_cur.close()

    # Test simple inserts to a leader/follower
    def test_leader_follower(self):
        # Create the oplog
        oplog = Oplog()

        # Create the table on leader and tell oplog about it
        self.session.create(self.uri, "key_format=S,value_format=S")
        t = oplog.add_uri(self.uri)

        # Create the follower and create its table
        # To keep this test relatively easy, we're only using a single URI.
        conn_follow = self.wiredtiger_open('follower', self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="follower")')
        session_follow = conn_follow.open_session('')
        session_follow.create(self.uri, "key_format=S,value_format=S")

        # Create some oplog traffic, a mix of inserts and updates
        oplog.insert(t, 900)
        oplog.update(t, 100)
        oplog.insert(t, 900)
        oplog.update(t, 100)

        # Apply them to leader WT and checkpoint.
        oplog.apply(self, self.session, 0, 2000)
        oplog.check(self, self.session, 0, 2000)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')

        self.session.checkpoint()     # checkpoint 1
        checkpoint_count = 1
        self.check_checkpoint(checkpoint_count)

        # Add some more traffic
        oplog.insert(t, 900)
        oplog.update(t, 100)
        oplog.apply(self, self.session, 2000, 1000)
        oplog.check(self, self.session, 0, 3000)

        # On the follower -
        # Apply some entries, a bit more than checkpoint 1
        oplog.apply(self, session_follow, 0, 2100)

        self.pr(f'OPLOG: {oplog}')
        oplog.check(self, session_follow, 0, 2100)

        # Then advance the checkpoint and make sure everything is still good
        self.pr('advance checkpoint')
        self.disagg_advance_checkpoint(conn_follow)
        oplog.check(self, session_follow, 0, 2100)

        # Now go back to leader, checkpoint and insert more.
        # On follower apply some, advance.
        # Rinse and repeat.
        leader_pos = 3000
        follower_pos = 2100

        for i in range(1, 10):
            self.pr(f'iteration {i}')
            self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(oplog.last_timestamp())}')

            self.session.checkpoint()
            checkpoint_pos = leader_pos
            checkpoint_count += 1
            self.check_checkpoint(checkpoint_count)

            # Every few times have no data between checkpoints.
            if i % 3 != 0:
                # More traffic on leader, stay ahead because follower will advance past checkpoint
                # before picking up checkpoint.
                oplog.insert(t, 900)
                oplog.update(t, 100)
                oplog.apply(self, self.session, leader_pos, 1000)
                leader_pos += 1000

                # The check begins at 0, which means this test will have quadratic performance.
                # We don't always have to start at 0.
                oplog.check(self, self.session, 0, leader_pos)

            # On follower, apply oplog. Stay a little behind the leader, but
            # we always must be in front of the checkpoint.
            follower_new_pos = max(min(follower_pos, leader_pos - 900), checkpoint_pos)
            to_apply = follower_new_pos - follower_pos
            oplog.apply(self, session_follow, follower_pos, to_apply)
            follower_pos = follower_new_pos

            self.pr(f'checking follower from pos 0 to {follower_pos} before checkpoint pick-up')
            oplog.check(self, session_follow, 0, follower_pos)

            # advance checkpoint
            self.pr('advance checkpoint')
            self.disagg_advance_checkpoint(conn_follow)

            # The check begins at 0, which means this test will have quadratic performance.
            self.pr(f'checking follower from pos 0 to {follower_pos} after checkpoint pick-up')
            oplog.check(self, session_follow, 0, follower_pos)
