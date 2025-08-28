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

import os, wiredtiger, wttest, helper_disagg
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

def disagg_ignore_expected_output(testcase):
    testcase.ignoreStdoutPattern('WT_VERB_RTS|(wiredtiger_open:.*WT_VERB_METADATA)')

helper_disagg.disagg_ignore_expected_output = disagg_ignore_expected_output

logdir = "log"

# test_layered46.py
#    Test deleting local files on restart.
@disagg_test_class
class test_layered46(wttest.WiredTigerTestCase, DisaggConfigMixin):

    conn_config = (
        "statistics=(all),statistics_log=(wait=1,json=true,on_close=true),"
        + "disaggregated=(page_log=palm,lose_all_my_data=true),"
        + f"log=(enabled=true,path={logdir}),"
    )

    disagg_storages = gen_disagg_storages("test_layered46", disagg_only=True)

    # Make scenarios for different cloud service providers.
    scenarios = make_scenarios(disagg_storages)

    create_session_config = "key_format=S,value_format=S"
    uri = "layered:test_layered46"
    uri_local = "table:test_layered46local"

    def wiredtiger_open(self, *args, **kwargs):
        os.makedirs(logdir, exist_ok=True)
        return super().wiredtiger_open(*args, **kwargs)

    # Load the page log extension, which has object storage support.
    def conn_extensions(self, extlist):
        if os.name == "nt":
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    def test_layered46(self):
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Create the tables
        self.session.create(self.uri, self.create_session_config)
        self.session.create(self.uri_local, self.create_session_config)

        # Put data to tables with checkpoints to ensure that we generate some history.
        for value in ["aaa", "bbb", "ccc"]:
            cursor = self.session.open_cursor(self.uri, None, None)
            cursor["A"] = value
            cursor.close()
            cursor = self.session.open_cursor(self.uri_local, None, None)
            cursor["A"] = value
            cursor.close()
            self.session.checkpoint()

        # Reopen the connection.
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.close_conn()

        with self.expectedStdoutPattern("Removing local file"):
            self.open_conn()

        self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Check the data.
        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor["A"], "ccc")
        cursor.close()

        # The local table should not exist.
        self.assertRaisesException(wiredtiger.WiredTigerError,
                                   lambda: self.session.open_cursor(self.uri_local, None, None))
