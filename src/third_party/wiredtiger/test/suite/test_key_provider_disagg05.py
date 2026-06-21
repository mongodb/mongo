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

from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_key_provider import KeyProviderBase
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# Push-mode key provider: exercise the pending-key list across multiple checkpoints and verify
# each checkpoint persists a fully valid key-provider page holding the selected key.
@disagg_test_class
class test_key_provider_disagg05(KeyProviderBase):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = f"layered:{test_name}"

    def populate_table(self):
        # A populated table gives every checkpoint real work to flush.
        self.dataset = SimpleDataSet(self, self.uri, 10)
        self.dataset.populate()
        self.row = 10

    def write_and_checkpoint(self):
        # A checkpoint only runs the key provider when there is dirty data to flush.
        self.row += 1
        cursor = self.session.open_cursor(self.uri)
        cursor[self.dataset.key(self.row)] = self.dataset.value(self.row)
        cursor.close()
        self.session.checkpoint()

    def test_multiple_pushes_across_checkpoints(self):
        self.populate_table()

        # Queue three keys, then advance stable to 3 so the checkpoint selects the most recent
        # (timestamp 3) and drains the consumed entries.
        self.push_crypt_key(1)
        self.push_crypt_key(2)
        self.push_crypt_key(3)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(3))
        self.write_and_checkpoint()
        self.validate_latest_kek(3)

        # Push a single key and advance stable to it: the checkpoint persists it (timestamp 4).
        self.push_crypt_key(4)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(4))
        self.write_and_checkpoint()
        self.validate_latest_kek(4)

        # Push another key and advance stable to it: the checkpoint persists it (timestamp 5).
        self.push_crypt_key(5)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        self.write_and_checkpoint()
        self.validate_latest_kek(5)

    def test_select_highest_at_or_below_stable(self):
        self.populate_table()

        # Queue three keys but advance stable only to 2: the checkpoint must select the highest
        # key at or below stable (timestamp 2), leaving the timestamp-3 key pending.
        self.push_crypt_key(1)
        self.push_crypt_key(2)
        self.push_crypt_key(3)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.write_and_checkpoint()
        self.validate_latest_kek(2)

        # Advance stable to 3: the still-pending timestamp-3 key is now selected.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(3))
        self.write_and_checkpoint()
        self.validate_latest_kek(3)
