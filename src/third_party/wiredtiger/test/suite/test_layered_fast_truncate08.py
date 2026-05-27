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
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin
from wtscenario import make_scenarios
import wttest


@disagg_test_class
class test_layered_fast_truncate08(LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__

    disagg_storages = gen_disagg_storages(test_name, disagg_only=True)
    scenarios = make_scenarios(disagg_storages)
    conn_config = 'disaggregated=(role="leader"),'

    uri = f"layered:{test_name}"

    def session_create_config(self):
        return "key_format=i,value_format=u"

    def populate(self, keys, value=b"v"):
        with closing(self.session.open_cursor(self.uri)) as cursor:
            with self.transaction():
                for key in keys:
                    cursor[key] = value

    def setup_layered_table(self):
        # Create the table and produce the initial checkpoint that the follower
        # will attach to.
        self.setup_leader()

    def setup_follower(self, keys=range(100)):
        super().setup_follower()
        # Add updates on the ingest that can be truncated later.
        self.populate(keys)

    def get_values(self, uri, start_key, stop_key):
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
        self.setup_layered_table()
        self.setup_follower()

        # Truncate a range of keys.
        start_key = 20
        stop_key = 80
        self.truncate(start_key, stop_key)

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
