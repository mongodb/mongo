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

import threading, time, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered70.py
#    Test we skip write pages if reconciliation doesn't make progress.
@disagg_test_class
class test_layered70(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader"),precise_checkpoint=true,page_delta=(leaf_page_delta=false),'

    create_session_config = 'key_format=i,value_format=S'

    uri = "layered:test_layered70"

    disagg_storages = gen_disagg_storages('test_layered70', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    def test_skip_write_full_page(self):
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(1)}')
        self.session.create(self.uri, self.create_session_config)

        # Insert an unstable key
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor[1] = 'value'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(10)}')

        # Verify checkpoint writes nothing to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_page_full_image_leaf: False,
        }, self.uri)

        # Verify again checkpoint writes nothing to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_page_full_image_leaf: False,
        }, self.uri)

        # Make the update stable
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(10)}')

        # Verify checkpoint writes a page
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_page_full_image_leaf: True,
        }, self.uri)

        # Insert another unstable key
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor[1] = 'value2'
        self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(20)}')

        # Verify again checkpoint writes nothing to disk
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_page_full_image_leaf: False,
        }, self.uri)

        # Make the update stable
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(20)}')

        # Verify checkpoint writes a page
        self.checkpoint_and_verify_stats({
            wiredtiger.stat.dsrc.rec_page_full_image_leaf: True,
        }, self.uri)
