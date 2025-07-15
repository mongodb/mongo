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
# test_layered41.py
#   Test dupicate key return values.

import wttest, wiredtiger
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered41.py
#    Test duplicate key.
@disagg_test_class
class test_layered41(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = ',disaggregated=(page_log=palm),'

    create_session_config = 'key_format=S,value_format=S'

    role = [
        ('leader', dict(role='leader')),
        ('follower', dict(role='follower')),
    ]

    disagg_storages = gen_disagg_storages('test_layered41', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, role)

    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + f'disaggregated=(role="{self.role}")'

    def test_dup_key(self):
        uri = "layered:test_layered41"
        self.session.create(uri, "key_format=S,value_format=S")

        c = self.session.open_cursor(uri, None, 'overwrite=false')
        for i in range(0, 100):
            self.session.begin_transaction()
            c[str(i)] = str(i)
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(1)}')

        c.set_key(str(10))
        c.set_value(str(20))
        self.assertRaisesHavingMessage(
            wiredtiger.WiredTigerError, lambda:c.insert(), '/WT_DUPLICATE_KEY/')
        self.assertEqual(c.get_value(), str(10))
