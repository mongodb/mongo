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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered66.py
#    Test evicting pages that haven't been materialized when closing a file
#    returns an error.
@disagg_test_class
class test_layered66(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'

    create_session_config = 'key_format=i,value_format=S'

    uri = "layered:test_layered66"

    disagg_storages = gen_disagg_storages('test_layered66', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    def test_layered66(self):
        page_log = self.conn.get_page_log(self.vars.page_log)
        self.session.create(self.uri, self.create_session_config)

        # Insert a key.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'value1'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))

        # Do a checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

        # Update the last materialized LSN to the checkpoint LSN.
        (ret, checkpoint_last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)

        page_log.pl_set_last_materialized_lsn(self.session, checkpoint_last_lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, checkpoint_last_lsn)

        # Insert another key.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri)
        cursor[2] = 'value1'
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        # Do a second checkpoint.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.checkpoint()

        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            self.session.verify(self.uri))

        # Update the last materialized LSN to the checkpoint LSN.
        (ret, checkpoint_last_lsn) = page_log.pl_get_last_lsn(self.session)
        self.assertEqual(ret, 0)

        page_log.pl_set_last_materialized_lsn(self.session, checkpoint_last_lsn)
        self.conn.set_context_uint(wiredtiger.WT_CONTEXT_TYPE_LAST_MATERIALIZED_LSN, checkpoint_last_lsn)

        # Verify should now succeed.
        self.session.verify(self.uri)
