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

import wttest
import wiredtiger
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered14.py
# Simple testing for layered random cursor
@disagg_test_class
class test_layered14(wttest.WiredTigerTestCase, DisaggConfigMixin):

    conn_base_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(page_log=palm),'
    disagg_storages = gen_disagg_storages('test_layered14', disagg_only = True)
    uri = "layered:test_layered14"
    nitems = 1000

    scenarios = make_scenarios(disagg_storages)

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="leader")'

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        DisaggConfigMixin.conn_extensions(self, extlist)

    def test_layered_random_cursor(self):
        self.session.create(self.uri, "key_format=S,value_format=S")

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "aaaa"

        for i in range(self.nitems):
            cursor[str(i)] = value1

        # XXX
        # Inserted timing delays around reopen, apparently needed because of the
        # layered table watcher implementation
        import time
        time.sleep(1.0)
        self.session.checkpoint()

        for i in range(self.nitems, 2 * self.nitems):
            cursor[str(i)] = value1

        cursor.close()

        random_cursor = self.session.open_cursor(self.uri, None, "next_random=true")
        self.assertEqual(random_cursor.next(), 0)
        random_cursor.close()

        # XXX
        # Inserted timing delays around reopen, apparently needed because of the
        # layered table watcher implementation
        import time
        time.sleep(1.0)
        follower_config = self.conn_base_config + 'disaggregated=(role="follower",' +\
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        self.reopen_conn(config = follower_config)
        time.sleep(1.0)

        random_cursor = self.session.open_cursor(self.uri, None, "next_random=true")
        self.assertEqual(random_cursor.next(), 0)
        random_cursor.close()


    def test_empty_table(self):
        self.session.create(self.uri, "key_format=S,value_format=S")

        random_cursor = self.session.open_cursor(self.uri, None, "next_random=true")
        self.assertEqual(random_cursor.next(), wiredtiger.WT_NOTFOUND)
        random_cursor.close()
