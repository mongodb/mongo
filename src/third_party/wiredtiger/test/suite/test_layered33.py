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

import os, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered33.py
#    Test delete on the ingest table.
@disagg_test_class
class test_layered33(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(page_log=palm,lose_all_my_data=true),'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    format_values = [
        ('string', dict(value_format='S')),
        ('integer', dict(value_format='I')),
    ]

    disagg_storages = gen_disagg_storages('test_layered33', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, format_values)

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    def value(self, v):
        if self.value_format == 'S':
            return str(v)
        return v

    def test_delete(self):
        uri = "layered:test_layered33"
        self.session.create(uri, f'key_format=S,value_format={self.value_format}')

        cursor = self.session.open_cursor(uri, None, None)
        for i in range(0, 100):
            cursor[str(i)] = self.value(i)

        for i in range(0, 100):
            cursor.set_key(str(i))
            self.assertEqual(cursor.remove(), 0)

        cursor.reset()

        # Ensure there is nothing in the table
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)
        for i in range(0, 100):
            cursor.set_key(str(i))
            self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
