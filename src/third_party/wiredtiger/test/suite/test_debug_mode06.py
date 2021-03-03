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

import wttest, wiredtiger

# test_debug_mode06.py
#   Test the debug mode settings. Test slow_checkpoint use (from WT-4981).
#   Note: testing timing will make results unreliable so we won't do that.
class test_debug_mode06(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled=true),debug_mode=(slow_checkpoint=true),statistics=(all)'
    uri = 'file:test_debug_mode06'

    # Insert some data to ensure setting/unsetting the flag does not
    # break existing functionality
    def insert_data(self, assert_time=0):
        self.session.create(self.uri, 'key_format=s,value_format=s')
        self.cursor = self.session.open_cursor(self.uri, None)
        self.cursor['key'] = 'value'
        self.cursor.close()
        self.session.checkpoint()

        # Validate checkpoint time if asked for.
        if (assert_time > 0):
            stat_cur = self.session.open_cursor('statistics:', None, None)
            checkpoint_time = int(stat_cur[wiredtiger.stat.conn.txn_checkpoint_time_recent][2])
            self.assertTrue(checkpoint_time >= assert_time)
            stat_cur.close()

    # Make flag works when set.
    def test_slow_checkpoints(self):
        # Ensure the checkpoint takes at least 10ms (the delay we have set).
        self.insert_data(10)

    # Make sure the flag can be 'turned off' as well.
    def test_slow_checkpoints_off(self):
        conn_reconfig = 'debug_mode=(slow_checkpoint=false)'
        self.conn.reconfigure(conn_reconfig)
        self.insert_data()

if __name__ == '__main__':
    wttest.run()
