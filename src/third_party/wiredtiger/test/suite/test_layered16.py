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

import os, os.path, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered16.py
#    Test layered table modify
@disagg_test_class
class test_layered16(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_config = 'statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                + 'disaggregated=(page_log=palm,role="leader"),'

    create_session_config = 'key_format=S,value_format=S'

    disagg_storages = gen_disagg_storages('test_layered16', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    num_restarts = 0

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_RTS')

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    def test_modify(self):
        uri = "layered:test_layered16"
        self.session.create(uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(uri)

        value1 = 'abcedfghijklmnopqrstuvwxyz' * 5
        # Insert a full value.
        self.session.begin_transaction()
        cursor[str(1)] = value1
        self.session.commit_transaction()

        # Insert a modify
        self.session.begin_transaction()
        cursor.set_key(str(1))
        cursor.modify([wiredtiger.Modify('A', 130, 0)])
        self.assertEqual(cursor.get_value(),  value1 + 'A')
        self.session.commit_transaction()

        # Validate that we do see the correct value.
        self.assertEqual(cursor[str(1)],  value1 + 'A')

        # Insert a second modify
        self.session.begin_transaction()
        cursor.set_key(str(1))
        cursor.modify([wiredtiger.Modify('B', 131, 0)])
        self.assertEqual(cursor.get_value(),  value1 + 'AB')
        self.session.commit_transaction()

        # Validate that we do see the correct value.
        self.assertEqual(cursor[str(1)],  value1 + 'AB')
