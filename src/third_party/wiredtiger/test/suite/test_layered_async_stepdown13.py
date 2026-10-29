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

# test_layered_async_stepdown13.py
#    A table created inside the step-down window has no stable constituent. Once the leader has
#    completed a checkpoint, a lookup that misses in ingest must still report not found rather than
#    attempt a follower-style stable open, which the leader-era snapshot would refuse.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_stepdown import LayeredStepdownMixin
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_async_stepdown13(LayeredStepdownMixin, wttest.WiredTigerTestCase):
    test_name = __qualname__

    conn_config = 'precise_checkpoint=true,disaggregated=(role="leader")'
    table_config = 'key_format=S,value_format=S'
    uri = f'layered:{test_name}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    def create_window_table_after_checkpoint(self):
        self.set_global_ts(1, 10)
        self.session.checkpoint()
        self.set_step_down_ts(20)
        self.session.create(self.uri, self.table_config)
        self.assertFalse(self.stable_constituent_exists(self.conn, self.uri))

    def test_search_miss_is_not_found(self):
        self.create_window_table_after_checkpoint()
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor.set_key('missing')
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()

        # A present key is still found through the ingest constituent.
        self.session.begin_transaction()
        cursor['present'] = 'value'
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(30))
        self.session.begin_transaction()
        cursor.set_key('present')
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.get_value(), 'value')
        cursor.set_key('missing')
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
        cursor.close()

    def test_modify_miss_is_not_found(self):
        self.create_window_table_after_checkpoint()
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor.set_key('missing')
        self.assertEqual(cursor.modify([wiredtiger.Modify('x', 0, 1)]), wiredtiger.WT_NOTFOUND)
        self.session.rollback_transaction()
        cursor.close()
