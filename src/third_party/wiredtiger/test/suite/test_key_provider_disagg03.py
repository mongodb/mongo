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

# Push-mode key provider smoke test: verify the pushed key is durably persisted to the turtle
# key-provider page after a checkpoint, and that the on-disk crypt page is fully valid.
@disagg_test_class
class test_key_provider_disagg03(KeyProviderBase):
    test_name = __qualname__
    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    uri = f"layered:{test_name}"

    def test_set_key_persists(self):
        ds = SimpleDataSet(self, self.uri, 10)
        ds.populate()

        # Push a key at timestamp 1, then advance stable to it so the checkpoint selects and
        # persists the pushed key to the key-provider page.
        self.push_crypt_key(1)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

        self.assertGreaterEqual(self.key_provider_page_count(), 1)
        self.validate_turtle_page()
        self.validate_latest_kek(1)
