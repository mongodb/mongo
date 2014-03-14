#!/usr/bin/env python
#
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

# test_reconfig.py
#    Smoke-test the connection reconfiguration operations.
class test_reconfig(wttest.WiredTigerTestCase):

    def test_reconfig_shared_cache(self):
        self.conn.reconfigure("shared_cache=(name=pool,size=300M)")

    def test_reconfig_statistics(self):
        self.conn.reconfigure("statistics=(all)")
        self.conn.reconfigure("statistics=(fast)")
        self.conn.reconfigure("statistics=(none)")

    def test_reconfig_verbose(self):
        # we know the verbose output format may change in the future,
        # so we just match on a string that's likely to endure.
        with self.expectedStdoutPattern('mutex: '):
            self.conn.reconfigure("verbose=[mutex]")
            # Reopening the connection allows the initial connection
            # to completely close and all its threads to finish.
            # If we don't do this, some trailing threads give additional
            # output after we make our 'expectedStdoutPattern' check,
            # and cause the test to fail.
            self.reopen_conn()

if __name__ == '__main__':
    wttest.run()
