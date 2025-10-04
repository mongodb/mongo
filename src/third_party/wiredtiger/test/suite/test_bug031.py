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
import wttest
from wtscenario import make_scenarios

# test_bug031.py
# This tests for the scenario discovered in WT-10717 with WT-10522 reverted.
# Without WT-10522, it is possible to miss an update when constructing the update list when there is
# an existing aborted update with a WT_UPDATE_RESTORED_FROM_DS flag.
class test_bug_031(wttest.WiredTigerTestCase):
    format_values = [
        ('column', dict(key_format='r', value_format='S')),
        ('column_fix', dict(key_format='r', value_format='8t')),
        ('row_integer', dict(key_format='i', value_format='S')),
    ]

    scenarios = make_scenarios(format_values)

    def test_bug031(self):
        uri = "table:test_bug031"

        key = 1
        if self.value_format == '8t':
            value = 1
        else:
            value = "1"

        self.session.create(uri, 'key_format={},value_format={}'.format(
            self.key_format, self.value_format))
        cursor = self.session.open_cursor(uri)

        # Perform a first insertion.
        self.session.begin_transaction()
        cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))

        # Set the stable and oldest timestamps.
        self.conn.set_timestamp('oldest_timestamp={},stable_timestamp={}'.format(
            self.timestamp_str(1), self.timestamp_str(10)))

        # Remove the record.
        self.session.begin_transaction()
        cursor.set_key(key)
        cursor.remove()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(20))

        # Evict everything.
        evict_cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()

        # After eviction, we have:
        # - Nothing in memory.
        # - A record with a start ts 10 and stop ts 20 in the DS.
        # - Nothing in the HS.

        # Insert an update.
        self.session.begin_transaction()
        cursor[key] = value
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        cursor.close()

        # Checkpoint.
        self.session.checkpoint()

        # After the checkpoint, we expect the following:
        # - In memory, an update list with insert @ 30 (U3), remove @ 20 (U2) and insert @ 10 (U1).
        # The two updates U1 and U2 should have the flag WT_UPDATE_PREPARE_RESTORED_FROM_DS as they
        # were read back into memory from disk by the checkpoint.
        # - The most recent insertion U3 in the DS.
        # - A record with a start ts 10 and stop ts 20 in the HS.

        # RTS.
        self.reopen_conn()

        cursor = self.session.open_cursor(uri)

        # Since the stable timestamp is 10, RTS marks all the updates performed at a time greater
        # time than 10 as aborted. The update chain in memory is:
        # U3 (aborted) -> U2 (aborted) -> U1.
        # The DS now contains the insert @ 10 (U1) and the HS is empty.

        # Insert but don't commit yet.
        self.session.begin_transaction()
        cursor[key] = value

        # Evict everything. Since there is an uncommitted update on the key, this performs update
        # restore for that key.
        evict_cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()

        # Since only the insert @ 10 (U1) is (re)written to disk, we now only have the two aborted
        # updates in memory: U3 (aborted) -> U2 (aborted). The rest remains unchanged.

        # Now commit a new insertion.
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(40))

        # We now have an additional insert @ 40 (U4) in the update list in memory on top of the two
        # aborted updates: U4 -> U3 (aborted) -> U2 (aborted)

        # Evict everything.
        # This eviction is supposed to move U4 to the DS and U1 in the HS.
        # However, without the fix in WT-10522, U1 is not moved to the HS because of the early exit
        # in __rec_append_orig_value when processing U2 as it has the flag
        # WT_UPDATE_RESTORED_FROM_DS.
        # With the fix from WT-10522, since U2 is aborted, we skip it, avoid the early exit and
        # restore the on disk update which is U1.
        evict_cursor = self.session.open_cursor(uri, None, 'debug=(release_evict)')
        evict_cursor.set_key(key)
        evict_cursor.search()
        evict_cursor.reset()
        evict_cursor.close()

        # Read update at timestamp 10.
        # Without the fix from WT-10522, this returns WT_NOTFOUND as the update was lost during the
        # previous eviction.
        self.session.begin_transaction('read_timestamp=' + self.timestamp_str(10))
        cursor.set_key(key)
        self.assertEqual(cursor.search(), 0)
