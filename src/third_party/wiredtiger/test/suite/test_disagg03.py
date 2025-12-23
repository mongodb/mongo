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

import wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_disagg03.py
# Test that creating a tiered table fails when in disaggregated storage mode
@disagg_test_class
class test_disagg03(wttest.WiredTigerTestCase, DisaggConfigMixin):

    role_scenarios = [
        ('leader', dict(role='leader')),
        ('follower', dict(role='follower')),
    ]
    prefix_scenarios = [
        ('tiered', dict(prefix='tiered:')),
        ('tier', dict(prefix='tier:')),
        ('object', dict(prefix='object:')),
    ]
    disagg_storages = gen_disagg_storages('test_disagg03', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, role_scenarios, prefix_scenarios)
    conn_base_config = 'statistics=(all),verbose=(tiered),'

    def conn_config(self):
        return self.conn_base_config + f'disaggregated=(role="{self.role}"),' \
            + 'disaggregated=(lose_all_my_data=true)'


    def test_disagg_tiered_disabled(self):
        # Test that we do not start the tiered storage worker thread in
        # disaggregated storage mode.

        self.captureout.checkAdditionalPattern(self,
            'Tiered storage not started: disaggregated storage.')

    def test_disagg_tiered_create_disabled(self):
        # Test that creating a tiered table fails in disaggregated storage mode.
        self.captureout.checkAdditionalPattern(self,
            'Tiered storage not started: disaggregated storage.')

        # Hit ENOTSUP with "tier", "tiered", and tiered "object" uri prefix
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
        lambda: self.session.create(self.prefix + 'test_disagg03',
            'key_format=S,value_format=S'),
            '/Operation not supported/')
