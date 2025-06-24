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
#
# test_sweep06.py
# This test confirms that table dhandles are correctly retained and not
# marked for sweep server expiry when file data dhandles are present.

from suite_subprocess import suite_subprocess
import wttest, threading
from wiredtiger import stat
from wtscenario import make_scenarios

class test_sweep06(wttest.WiredTigerTestCase, suite_subprocess):
    dhandles = 200
    format='key_format=i,value_format=S'
    tablebase = 'test_sweep06'
    uri = 'table:' + tablebase
    conn_config = 'file_manager=(close_handle_minimum=0,' + \
                  'close_idle_time=60,close_scan_interval=30),session_max=530,' + \
                  'verbose=(sweep:3)'

    cursor_caching = [
        ('cursor_caching_disabled', dict(cursor_caching=False)),
        ('cursor_caching_enabled', dict(cursor_caching=True)),
    ]

    scenarios = make_scenarios(cursor_caching)

    # We enabled verbose log level DEBUG_3 in this test to catch an invalid pointer in dhandle.
    # However, this also causes the log line 'session dhandle name' to appear, which we want to ignore.
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.ignoreStdoutPattern('WT_VERB_SWEEP')

    def insert(self, i, start, rows):
        session = self.conn.open_session()
        uri = self.uri + str(i)
        cursor = session.open_cursor(uri)

        session.begin_transaction()
        for i in range(start, rows):
            cursor.set_key(i)
            cursor.set_value(str(i))
            cursor.insert()
        session.commit_transaction()
        cursor.close()
        session.close()

    def test_dhandles(self):
        if self.cursor_caching:
            self.session.reconfigure('cache_cursors=true')
        for i in range(1,self.dhandles):
            uri = self.uri + str(i)
            self.session.create(uri, self.format)

        for i in range(1,100):
            threads = []
            for i in range(1,self.dhandles):
                thread = threading.Thread(target=self.insert, args=(i, 0, 100))
                thread.start()
                threads.append(thread)

            for thread in threads:
                thread.join()

        stat_cursor = self.session.open_cursor('statistics:', None, None)
        close1 = stat_cursor[stat.conn.dh_sweep_dead_close][2]
        close2 = stat_cursor[stat.conn.dh_sweep_expired_close][2]
        stat_cursor.close()

        # The expectation is that no dhandles have been closed.
        self.assertEqual(close1, 0)
        self.assertEqual(close2, 0)
