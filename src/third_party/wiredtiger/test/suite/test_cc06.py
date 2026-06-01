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
# [TEST_TAGS]
# checkpoint:checkpoint_cleanup
# [END_TAGS]

import time, wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# test_cc06.py
# Exercise the checkpoint_cleanup.use_thread config option.
#
# cc_handle_processed is reset to 0 at the start of every checkpoint and then
# incremented per row-store/VLCS dhandle whose cleanup runs during that
# checkpoint. cc_success is bumped only when the dedicated cleanup thread
# completes a cycle. Together they distinguish the two paths:
#
#  - use_thread=false (default): inline cleanup during the checkpoint tree
#    walk. cc_handle_processed > 0 immediately after session.checkpoint();
#    the thread isn't running, so cc_success stays at 0.
#  - use_thread=true: a plain checkpoint resets cc_handle_processed to 0 and
#    no thread is signaled, so it stays at 0. Triggering the thread via
#    debug=(checkpoint_cleanup=true) bumps both cc_success and
#    cc_handle_processed once the thread has run.
class test_cc06(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('inline', dict(use_thread='false')),
        ('thread', dict(use_thread='true')),
    ])

    def conn_config(self):
        # wait=60 is the minimum; we rely on the debug trigger rather than the
        # periodic timer when use_thread=true.
        return ('cache_size=50MB,statistics=(all),'
                'checkpoint_cleanup=(use_thread=%s,wait=60)' % self.use_thread)

    def get_stat(self, statid):
        c = self.session.open_cursor('statistics:')
        v = c[statid][2]
        c.close()
        return v

    def test_use_thread(self):
        nrows = 5000
        uri = 'table:cc06'
        self.session.create(uri, 'key_format=i,value_format=S')
        value = 'a' * 100

        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows + 1):
            cursor[i] = value
        cursor.close()

        if self.use_thread == 'false':
            # Inline path: cleanup runs during the checkpoint tree walk, so
            # cc_handle_processed is non-zero immediately after the call. The
            # dedicated thread isn't started, so cc_success must stay at 0.
            self.session.checkpoint()
            self.assertGreater(
                self.get_stat(stat.conn.cc_handle_processed), 0)
            self.assertEqual(self.get_stat(stat.conn.cc_success), 0)
        else:
            # Thread path: a plain checkpoint must not drive cleanup. The
            # checkpoint itself resets cc_handle_processed to 0 and nothing
            # else bumps it.
            self.session.checkpoint()
            self.assertEqual(
                self.get_stat(stat.conn.cc_handle_processed), 0)
            self.assertEqual(self.get_stat(stat.conn.cc_success), 0)

            # Signal the cleanup thread and wait for one cycle to complete.
            self.session.checkpoint('debug=(checkpoint_cleanup=true)')
            deadline = time.time() + 30
            while (self.get_stat(stat.conn.cc_success) == 0
                   and time.time() < deadline):
                time.sleep(0.1)
            self.assertGreater(self.get_stat(stat.conn.cc_success), 0)
            self.assertGreater(
                self.get_stat(stat.conn.cc_handle_processed), 0)

if __name__ == '__main__':
    wttest.run()
