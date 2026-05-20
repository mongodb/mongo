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

# test_layered_fast_truncate08.py
#   On a follower, range truncate must represent each delete as a standard
#   update carrying the layered tombstone sentinel (\x14\x14) into the ingest
#   file, rather than using cursor->remove() to create a WT_UPDATE_TOMBSTONE.

from contextlib import closing
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
import wttest


@disagg_test_class
class test_layered_fast_truncate08(wttest.WiredTigerTestCase):
    test_name = __qualname__

    disagg_storages = gen_disagg_storages(test_name, disagg_only=True)
    scenarios = make_scenarios(disagg_storages)
    conn_config = 'disaggregated=(role="leader"),'

    def setup_layered_table(self, layered_uri: str):
        # Create the table and produce the initial checkpoint that the follower
        # will attach to.
        session_config = "key_format=i,value_format=u"
        self.session.create(layered_uri, session_config)
        self.session.checkpoint()

    def setup_follower(self, layered_uri: str):
        self.reopen_disagg_conn('disaggregated=(role="follower"),')

        # Add updates on the ingest that can be truncated later.
        with closing(self.session.open_cursor(layered_uri)) as cursor:
            with self.transaction():
                for i in range(100):
                    cursor[i] = b"v"

    def truncate(self, layered_uri: str, start_key: int, stop_key: int):
        # Truncate between start and stop keys inclusive.
        with (
            closing(self.session.open_cursor(layered_uri)) as start_cursor,
            closing(self.session.open_cursor(layered_uri)) as stop_cursor,
        ):
            start_cursor.set_key(start_key)
            stop_cursor.set_key(stop_key)

            with self.transaction():
                self.session.truncate(None, start_cursor, stop_cursor, None)

    def get_values(self, uri: str, start_key: int, stop_key: int):
        # Return values of any keys between start and stop inclusive that exist.
        values = []

        with closing(self.session.open_cursor(uri)) as cursor:
            for i in range(start_key, stop_key + 1):
                cursor.set_key(i)
                if cursor.search() == 0:
                    values.append(cursor.get_value())

        return values

    def test_follower_truncate_writes_tombstone_to_ingest(self):
        # Set up a follower with existing ingest updates.
        layered_uri = f"layered:{self.test_name}"
        self.setup_layered_table(layered_uri)
        self.setup_follower(layered_uri)

        # Truncate a range of keys.
        start_key = 20
        stop_key = 80
        self.truncate(layered_uri, start_key, stop_key)

        # Examine what the truncate actually wrote to the ingest file.
        ingest_uri = f"file:{self.test_name}.wt_ingest"
        values = self.get_values(ingest_uri, start_key, stop_key)

        # If WT_UPDATE_TOMBSTONE had been used, a cursor search on the ingest
        # file would not find any of the truncated keys.
        num_keys_truncated = stop_key - start_key + 1
        self.assertEqual(len(values), num_keys_truncated)

        # The truncated keys should instead be represented by the layered
        # tombstone sentinel value in the ingest file.
        sentinel = b"\x14\x14"
        expected_values = [sentinel] * num_keys_truncated
        self.assertEqual(values, expected_values)


if __name__ == "__main__":
    wttest.run()
