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

import time
import wttest
from wiredtiger import stat

# When a connection is configured with logging and a log-size based checkpoint server, the final
# checkpoint performed during close can still write enough log to cross the configured threshold. At
# that point the checkpoint server has already been destroyed and its condition variable is NULL;
# signaling it causes a null-pointer dereference. This test exercises the shutdown path with the
# offending configuration.
class test_bug036(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'table:{test_name}'
    conn_config = 'log=(enabled=true,file_max=102400),checkpoint=(log_size=100,wait=0)'

    def test_bug036_checkpoint_log_size_close(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        stat_cursor = self.session.open_cursor('statistics:')
        initial_ckpts = stat_cursor[stat.conn.checkpoints_total_succeed][2]
        stat_cursor.close()

        # Write enough log to trigger a checkpoint from the server thread, which resets the
        # log-size counter.
        c = self.session.open_cursor(self.uri)
        for i in range(250):
            c['k%04d' % i] = 'v' + 'x' * 900
        c.close()

        # Wait for the checkpoint server to run and reset the log-size counter.
        deadline = time.time() + 10
        while time.time() < deadline:
            stat_cursor = self.session.open_cursor('statistics:')
            now = stat_cursor[stat.conn.checkpoints_total_succeed][2]
            stat_cursor.close()
            if now > initial_ckpts:
                break
            time.sleep(0.05)
        self.assertGreater(
            now, initial_ckpts, 'checkpoint server did not run within the polling deadline')

        # Add a moderate amount of further work. The final checkpoint during close must flush
        # the resulting logs; with the bug this could attempt to signal the already-destroyed
        # checkpoint condition variable and crash.
        for t in range(40):
            extra = 'table:%s_extra_%d' % (self.test_name, t)
            self.session.create(extra, 'key_format=S,value_format=S')
            c = self.session.open_cursor(extra)
            c['a'] = 'b' * 100
            c.close()

        # Reopening closes and re-opens the connection. A crash in close will fail the test.
        self.reopen_conn()

if __name__ == '__main__':
    wttest.run()
