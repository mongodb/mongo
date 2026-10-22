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
#
# test_layered_prepare11.py
#   Preparing a transaction that truncated a range of a layered table on a follower aborts when the
#   connection is configured to preserve prepared transactions.

import os
import signal
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from helper_layered_fast_truncate import LayeredFastTruncateConfigMixin, range_inclusive
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

@disagg_test_class
class test_layered_prepare11(
        LayeredFastTruncateConfigMixin, wttest.WiredTigerTestCase, suite_subprocess):
    BASE_CONFIG = 'statistics=(all),precise_checkpoint=true,preserve_prepared=true,'
    conn_config = BASE_CONFIG + 'disaggregated=(role="leader"),'
    uri = f'layered:{__qualname__}'

    disagg_storages = gen_disagg_storages(disagg_only=True)
    scenarios = make_scenarios(disagg_storages)

    nitems = 100

    def test_prepared_follower_truncate_aborts(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        test_case = self.subprocess_prepared_follower_truncate
        test_case_name = f"{test_case.__module__}.{test_case.__qualname__}"
        home = self.crash_in_subprocess(test_case_name, test_case_name, signal.SIGABRT)
        self.check_file_contains(os.path.join(home, "stderr.txt"),
            "a prepared transaction cannot include a truncate entry when preserve_prepared is "
            "configured")

    def subprocess_prepared_follower_truncate(self):
        self.session.create(self.uri, self.session_create_config())
        self.populate(range_inclusive(1, self.nitems))
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()
        self.reopen_disagg_conn(self.BASE_CONFIG + 'disaggregated=(role="follower"),')

        start = self.session.open_cursor(self.uri)
        start.set_key(self.key(1))
        stop = self.session.open_cursor(self.uri)
        stop.set_key(self.key(self.nitems))
        self.session.begin_transaction()
        self.session.truncate(None, start, stop, None)
        start.close()
        stop.close()

        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(10) +
            ',prepared_id=' + self.prepared_id_str(1))
