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
from helper import simulate_crash_restart
from helper_key_provider import KeyProviderBase
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# Test basic pull key provider scenarios.
@disagg_test_class
class test_key_provider_disagg01(KeyProviderBase):
    test_name = __qualname__
    # Pull (get_key) model.
    key_provider_version = 0

    crash_value = [
        ('reopen', dict(crash=False)),
        ('crash', dict(crash=True)),
    ]

    disagg_storages = gen_disagg_storages(disagg_only = True)
    scenarios = make_scenarios(disagg_storages, crash_value)

    nentries = 1000
    uri = f"layered:{test_name}"

    def validate_number_elements(self, home="."):
        kek = self.key_provider_page_count(home)
        rows = self.sqlite_query(self.turtle_table,
            f'SELECT COUNT(*) AS c FROM pages WHERE table_id={self.WT_SPECIAL_PALI_TURTLE_FILE_ID};',
            home)
        turtle = rows[0]['c']
        if (self.key_expires == 0):
            self.assertEqual(kek, turtle)
        else:
            self.assertGreaterEqual(kek, turtle)

    def validate_meta_file(self, home="."):
        # The turtle references the main KEK page; the engine re-validates that page when it loads
        # the checkpoint on reopen.
        self.validate_turtle_page(home=home)

    def test_key_provider_disagg01(self):
        # Populate table.
        ds = SimpleDataSet(self, self.uri, self.nentries)
        ds.populate()
        ds.check()

        # Initiate checkpoint to trigger key provider semantics.
        self.session.checkpoint()
        self.validate_meta_file()

        # Initiate checkpoint again to trigger key provider semantics.
        self.session.checkpoint()
        self.validate_meta_file()

        first_row = ds.rows + 1
        ds.populate(first_row=first_row)
        ds.check()

        # Validate that key persists after crash/restart.
        self.key_expires = 43200  # Keys expire after 12 hours.
        if (self.crash):
            simulate_crash_restart(self, ".", "RESTART")
            self.validate_meta_file("RESTART")
            self.validate_number_elements("RESTART")
        else:
            self.reopen_conn()
            self.validate_meta_file()
            self.validate_number_elements()

        first_row = ds.rows + 1
        ds.populate(first_row=first_row)
        ds.check()

        # Initiate checkpoint and check for new key expiry.
        self.session.checkpoint()
        self.validate_meta_file()
