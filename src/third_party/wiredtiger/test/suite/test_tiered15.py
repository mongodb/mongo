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
#
# test_tiered15.py
#   Test the "type" configuration in session.create with tiered storage.

from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtscenario import make_scenarios
import wiredtiger, wttest

class test_tiered15(TieredConfigMixin, wttest.WiredTigerTestCase):
    types = [
        ('file', dict(type='file', tiered_err=False, non_tiered_err=False, non_tiered_errmsg=None)),
        ('table', dict(type='table', tiered_err=True, non_tiered_err=False, non_tiered_errmsg=None)),
        ('tier', dict(type='tier', tiered_err=True, non_tiered_err=False, non_tiered_errmsg=None)),
        ('tiered', dict(type='tiered', tiered_err=True, non_tiered_err=True, non_tiered_errmsg="/Invalid argument/")),
        ('colgroup', dict(type='colgroup', tiered_err=True, non_tiered_err=True, non_tiered_errmsg=None)),
        ('index', dict(type='index', tiered_err=True, non_tiered_err=True, non_tiered_errmsg="/Invalid argument/")),
        ('lsm', dict(type='lsm', tiered_err=True, non_tiered_err=False, non_tiered_errmsg=None)),
        ('backup', dict(type='backup', tiered_err=True, non_tiered_err=False, non_tiered_errmsg="/Operation not supported/")),
    ]

    # This is different to the self.is_tiered_scenario() value - that one configures tiered
    # storage at a connection level but here we want to configure at the table level.
    is_tiered_table = [
        ('tiered_table', dict(is_tiered_table=True)),
        ('nontiered_table', dict(is_tiered_table=False)),
    ]

    tiered_storage_sources = gen_tiered_storage_sources()
    scenarios = make_scenarios(tiered_storage_sources, types)

    def test_create_type_config(self):
        if not self.is_tiered_scenario():
            self.skipTest('The test should only test connections configured with tiered storage.')

        tiered_status = "tiered" if self.is_tiered_table else "nontiered"
        uri = "table:" + tiered_status + self.type

        if self.is_tiered_table:
            # Creating a tiered table with the type configuration set to file should succeed, and all other types
            # should not succeed.
            if self.type == "file":
                self.session.create(uri, "type=" + self.type)
            else:
                tiered_errmsg = "/Operation not supported/"
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.session.create(uri, "type=" + self.type), tiered_errmsg)
        else:
            # Creating a non-tiered table within a connection configured with tiered storage should allow the type
            # configuration to be set to some other types besides "file".
            if not self.non_tiered_err:
                self.session.create(uri, "tiered_storage=(name=none),type=" + self.type)
            else:
                # The types "tiered", "colgroup", "index", and "backup" are not supported when creating a non-tiered
                # table in a non-tiered connection. It is expected these types will also not work with non-tiered
                # tables in a connection configured with tiered storage.
                # Additionally, configuring type to "colgroup" causes WiredTiger to crash, skip this scenario.
                if self.type == "colgroup":
                    self.skipTest('Skip the colgroup type configuration as we expect it to crash.')
                self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.session.create(uri, "tiered_storage=(name=none),type=" + self.type), self.non_tiered_errmsg)

if __name__ == '__main__':
    wttest.run()
