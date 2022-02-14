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
# test_sweep02.py
# Test configuring and reconfiguring sweep options.
#

import wttest

class test_sweep02(wttest.WiredTigerTestCase):
    base_config = 'create,'
    dir = 'WT_TEST'
    tablebase = 'test_sweep02'
    uri = 'table:' + tablebase

    # Disable default setup/shutdown steps - connections are managed manually.
    def setUpSessionOpen(self, conn):
        return None

    def setUpConnectionOpen(self, dir):
        self.dir = dir
        return None

    def test_config01(self):
        self.conn = self.wiredtiger_open(self.dir,
            self.base_config + "file_manager=()")

    def test_config02(self):
        self.conn = self.wiredtiger_open(self.dir,
            self.base_config + "file_manager=(close_scan_interval=1)")

    def test_config03(self):
        self.conn = self.wiredtiger_open(self.dir,
            self.base_config + "file_manager=(close_idle_time=1)")

    def test_config04(self):
        self.conn = self.wiredtiger_open(self.dir,
            self.base_config + "file_manager=(close_handle_minimum=500)")

    def test_config05(self):
        self.conn = self.wiredtiger_open(self.dir, self.base_config + \
            "file_manager=(close_scan_interval=1,close_idle_time=1)")

if __name__ == '__main__':
    wttest.run()
