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

import os, time, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_layered21.py
#    Test the basic ability to insert on a follower.
@disagg_test_class
class test_layered21(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(page_log=palm),'
    def conn_config(self):
        return self.conn_base_config + f'disaggregated=(role="{self.initial_role}")'

    uri = "layered:test_layered21"
    nentries = 1000

    role_scenarios = [
        ('leader', dict(initial_role='leader')),
        ('follower', dict(initial_role='follower')),
    ]
    disagg_storages = gen_disagg_storages('test_layered21', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, role_scenarios)

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    # Test simple inserts to a leader/follower
    def test_insert_changing_roles(self):
        role = self.initial_role

        # No matter what the role, we should be able to insert and
        # see the results.
        ds = SimpleDataSet(self, self.uri, self.nentries)
        ds.populate()
        ds.check()

        if role == 'leader':
            # If we are the leader, we should be able to shut down and switch roles
            # in the same directory, insert some new items and see them.

            self.session.checkpoint()
            self.reopen_conn(config=self.conn_base_config +
                    f'disaggregated=(role="follower",checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")')

            first_row = ds.rows + 1
            ds.rows += 1000
            ds.populate(first_row=first_row)
            ds.check()
