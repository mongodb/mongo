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

# Toggle the key_provider API version across restarts and verify the database stays readable.

import wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

@disagg_test_class
class test_key_provider_disagg04(wttest.WiredTigerTestCase):
    test_name = __qualname__
    def conn_config(self):
        return self.extensionsConfig() + ',disaggregated=(role="leader")'

    start_versions = [
        ('v0', dict(start_version=0)),
        ('v1', dict(start_version=1)),
    ]

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages, start_versions)

    uri = f"layered:{test_name}"
    nentries = 200

    # Restart re-invokes conn_extensions, so flipping this field switches the loaded provider.
    current_version = 0

    DEFAULT_KEY_DATA = b'abcdefghijklmnopqrstuvwxyz'

    def conn_extensions(self, extlist):
        config = f'=(early_load=true,config=\"verbose=-1,version={self.current_version}\")'
        extlist.extension('test', "key_provider" + config)
        DisaggConfigMixin.conn_extensions(self, extlist)

    def checkpoint(self):
        # Push mode: a checkpoint only persists a pushed key once stable reaches its timestamp, so
        # push at a fresh timestamp then advance stable to it. Pull mode rotates keys through
        # get_key, so no push is needed there.
        if self.current_version == 1:
            self.push_ts += 1
            crypt = wiredtiger.CryptKeys()
            crypt.keys = self.DEFAULT_KEY_DATA
            crypt.timestamp = self.push_ts
            self.assertEqual(self.conn.get_key_provider().set_key(self.session, crypt), 0)
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(self.push_ts))
        self.session.checkpoint()

    def restart_with_version(self, version):
        self.current_version = version
        self.restart_without_local_files()

    def test_key_provider_version_toggle(self):
        if (self.ds_name != "palite"):
            self.skipTest("Must use PALite for disagg key provider testing")

        # Monotonic push timestamp for this test; advances on each push-mode checkpoint.
        self.push_ts = 0

        # The scenario runs start -> other -> start -> other, covering both directions.
        other_version = 1 - self.start_version

        # Part 1: start with the parameterized version, populate, checkpoint.
        self.current_version = self.start_version
        self.reopen_conn()

        ds1 = SimpleDataSet(self, self.uri, self.nentries)
        ds1.populate()
        self.checkpoint()
        ds1.check()

        # Part 2: restart on the other version, verify, add more.
        self.restart_with_version(other_version)
        ds1.check()

        ds2 = SimpleDataSet(self, self.uri, self.nentries * 2)
        ds2.populate(first_row=self.nentries + 1)
        self.checkpoint()
        ds2.check()

        # Part 3: restart back on the starting version.
        self.restart_with_version(self.start_version)
        ds2.check()

        ds3 = SimpleDataSet(self, self.uri, self.nentries * 3)
        ds3.populate(first_row=self.nentries * 2 + 1)
        self.checkpoint()
        ds3.check()

        # Part 4: final flip to the other version, verify all three batches.
        self.restart_with_version(other_version)
        ds3.check()
