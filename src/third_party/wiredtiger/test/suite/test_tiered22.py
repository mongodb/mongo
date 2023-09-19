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

import os, wiredtiger, wttest
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtscenario import make_scenarios

# test_tiered22.py
#    Check that compaction does not work on tiered tables.
class test_tiered22(TieredConfigMixin, wttest.WiredTigerTestCase):
    storage_sources = gen_tiered_storage_sources(wttest.getss_random_prefix(), 'test_tiered22',
                                                 tiered_only=True)
    scenarios = make_scenarios(storage_sources)

    def test_tiered22(self):
        uri = "table:tiered"
        uri_local_file = 'tiered-0000000001.wtobj'
        self.session.create(uri, "key_format=S,value_format=S")

        assert os.path.exists(uri_local_file)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                                     lambda: self.session.compact(uri_local_file, None),
                                     '/Operation not supported/')
        
if __name__ == '__main__':
    wttest.run()
