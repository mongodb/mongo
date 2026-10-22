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
# test_prepare52.py
#   Preparing a transaction that fast-truncated a range of pages aborts when the connection is
#   configured to preserve prepared transactions.

import os
import signal
import wiredtiger
import wttest
from suite_subprocess import suite_subprocess

class test_prepare52(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = 'precise_checkpoint=true,preserve_prepared=true,statistics=(all)'
    uri = 'table:test_prepare52'
    nrows = 10000

    def test_prepared_fast_truncate_aborts(self):
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        test_case = self.subprocess_prepared_fast_truncate
        test_case_name = f"{test_case.__module__}.{test_case.__qualname__}"
        home = self.crash_in_subprocess(test_case_name, test_case_name, signal.SIGABRT)
        self.check_file_contains(os.path.join(home, "stderr.txt"),
            "a prepared transaction cannot include a fast truncate when preserve_prepared is "
            "configured")

    def subprocess_prepared_fast_truncate(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        self.session.begin_transaction()
        for i in range(1, self.nrows + 1):
            cursor[i] = 'v' * 100
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(10))
        cursor.close()
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(10) +
            ',stable_timestamp=' + self.timestamp_str(10))
        self.session.checkpoint()

        # Start from a clean cache, so the truncated pages are on disk and can be fast-truncated.
        self.reopen_conn()

        start = self.session.open_cursor(self.uri)
        start.set_key(100)
        stop = self.session.open_cursor(self.uri)
        stop.set_key(self.nrows - 100)
        self.session.begin_transaction()
        self.session.truncate(None, start, stop, None)
        start.close()
        stop.close()

        # Fail with a Python error rather than the expected abort if no page was fast-truncated.
        stat_cursor = self.session.open_cursor('statistics:')
        fast_truncated = stat_cursor[wiredtiger.stat.conn.cache_truncate_txn_uncommitted_bytes][2]
        stat_cursor.close()
        self.assertGreater(fast_truncated, 0)

        self.session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20) +
            ',prepared_id=' + self.prepared_id_str(1))
