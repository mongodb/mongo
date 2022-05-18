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

from helper_tiered import TieredConfigMixin, tiered_storage_sources
import wiredtiger, wttest
from wtscenario import make_scenarios

# Test session.create with the exclusive configuration.
class test_create_excl(TieredConfigMixin, wttest.WiredTigerTestCase):
    types = [
        ('file', dict(type = 'file:')),
        ('table', dict(type = 'table:')),
    ]

    scenarios = make_scenarios(tiered_storage_sources, types)

    def test_create_excl(self):
        if self.is_tiered_scenario() and self.type == 'file:':
            self.skipTest('Tiered storage does not support file URIs.')

        uri = self.type + "create_excl"

        # Create the object with the exclusive setting.
        self.session.create(uri, "exclusive=true")

        # Exclusive re-create should error.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.create(uri, "exclusive=true"))

        # Non-exclusive re-create is allowed.
        self.session.create(uri, "exclusive=false")

        # Exclusive create on a table that does not exist should succeed.
        self.session.create(uri + "_non_existent", "exclusive=true")

        # Non-exclusive create is allowed.
        self.session.create(uri + "_non_existent1", "exclusive=false")

if __name__ == '__main__':
    wttest.run()
