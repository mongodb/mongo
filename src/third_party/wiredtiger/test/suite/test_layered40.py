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

import os, os.path, shutil, time, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered40.py
#    Test layered table metadata has logging disabled.
@wttest.skip_for_hook("tiered", "FIXME-WT-14938: crashing with tiered hook.")
@disagg_test_class
class test_layered40(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_config = 'log=(enabled=true),disaggregated=(page_log=palm,role="leader"),'

    create_session_config = 'key_format=S,value_format=S'

    layered_uris = ["table:test_layered40a", "layered:test_layered40b"]

    disagg_storages = gen_disagg_storages('test_layered40', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    # Ensure that the metadata cursor has all the expected URIs.
    def check_metadata_cursor(self):
        cursor = self.session.open_cursor('metadata:create', None, None)
        for key in self.layered_uris:
            cursor.set_key(key)
            self.assertEqual(cursor.search(), 0)
            self.assertTrue("log=(enabled=false)" in cursor.get_value())

    def test_layered40(self):
        # Create tables
        for uri in self.layered_uris:
            cfg = self.create_session_config
            if not uri.startswith('layered'):
                cfg += ',block_manager=disagg,type=layered'
            self.session.create(uri, cfg)

        self.check_metadata_cursor()
