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

import time
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from test_cc01 import test_cc_base
from wiredtiger import stat
from wtscenario import make_scenarios

# test_cc11.py
# Test that checkpoint cleanup is not run on follower.
@disagg_test_class
class test_cc11(DisaggConfigMixin, test_cc_base):
    disagg_storages = gen_disagg_storages('test_cc11', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    conn_config = 'disaggregated=(page_log=palm),cache_size=10MB,page_delta=(delta_pct=100),disaggregated=(role="follower"),checkpoint_cleanup=[wait=1,file_wait_ms=0],'

    def test_cc11(self):
        # Create a table.
        create_params = 'key_format=i,value_format=S'
        nrows = 10
        uri = "table:cc11"

        # Create and populate a table.
        self.session.create(uri, create_params)

        old_value = "a"
        old_ts = 1
        self.populate(uri, 0, nrows, old_value, old_ts)

        # Wait for the checkpoint cleanup thread to run
        time.sleep(1)

        # Trigger a checkpoint to initiate cleanup
        self.session.checkpoint('debug=(checkpoint_cleanup=true)')

        # Ensure checkpoint cleanup is not run
        c = self.session.open_cursor('statistics:')
        cc_run = c[stat.conn.checkpoint_cleanup_success][2]
        self.assertEqual(cc_run, 0)
