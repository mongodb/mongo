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

from time import sleep
import wttest, threading

# Prior to a bugfix in WiredTiger it was possible to read a partial transaction if the config
# ignore prepare was provided. This test demonstrates that case.
@wttest.skip_for_hook("tiered", "Fails with tiered storage")
class test_prepare28(wttest.WiredTigerTestCase):
    conn_config= 'timing_stress_for_test=[prepare_resolution_2]'
    uri = 'table:test_prepare28'
    numrows = 1
    value1 = 'aaaaa'
    value2 = 'bbbbb'
    value3 = 'ccccc'

    def test_ignore_prepare(self):
        self.session.create(self.uri, 'key_format=i,value_format=S')
        self.conn.set_timestamp('oldest_timestamp=' + self.timestamp_str(1))
        cursor = self.session.open_cursor(self.uri)
        # Prepare a value at timestamp 4
        self.session.begin_transaction()
        cursor[1] = self.value1
        cursor[1] = self.value2
        cursor[1] = self.value3
        self.session.prepare_transaction('prepare_timestamp=4')
        # Create a thread.
        ooo_thread = threading.Thread(target=self.read_update)
        # Start the thread
        ooo_thread.start()
        self.session.commit_transaction('commit_timestamp=6,durable_timestamp=6')

    def read_update(self):
        sleep(0.1)
        session = self.setUpSessionOpen(self.conn)
        cursor = session.open_cursor(self.uri)
        session.begin_transaction('ignore_prepare=true')
        cursor.set_key(1)
        session.breakpoint()
        # Read here
        ret = cursor.search()
        # Assert it didn't find anything, i.e. not WT_NOTFOUND
        assert(ret == -31803)
        session.commit_transaction()
