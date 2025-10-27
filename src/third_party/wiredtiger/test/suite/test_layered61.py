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

import os, os.path, shutil, threading, time, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered61.py
# Test the timestamps on the ingest tables are not cleared even they are globally
# visible.
@disagg_test_class
class test_layered61(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="follower")'

    create_session_config = 'key_format=S,value_format=S'
    uri = "layered:test_layered61"

    disagg_storages = gen_disagg_storages('test_layered61', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    def test_layered61(self):
        # Create a table with some data
        self.session.create(self.uri, self.create_session_config)
        cursor = self.session.open_cursor(self.uri, None, None)
        self.session.begin_transaction()
        cursor['a'] = 'b'
        self.session.commit_transaction(f"commit_timestamp={self.timestamp_str(10)}")
        cursor.close()

        self.conn.set_timestamp(f"stable_timestamp={self.timestamp_str(20)},oldest_timestamp={self.timestamp_str(20)}")

        # Evict the data.
        session = self.conn.open_session("debug=(release_evict_page)")
        evict_cursor = session.open_cursor(self.uri, None, None)
        session.begin_transaction()
        evict_cursor.set_key('a')
        evict_cursor.search()
        session.rollback_transaction()
        evict_cursor.close()
        session.close()

        # Step up
        self.conn.reconfigure('disaggregated=(role="leader")')
