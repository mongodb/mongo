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

# checkpoint_util.py
# Base class providing checkpoint-related helpers.
class checkpoint_util(wttest.WiredTigerTestCase):

    def wait_for_checkpoint_start(self, session=None, timeout=60, poll_interval=0.1):
        """
        Poll until a checkpoint is running (the checkpoint_state connection statistic becomes
        non-zero). Bounded so the test fails fast with a clear message instead of spinning to a
        task-level timeout if a checkpoint never starts. poll_interval controls how often the
        statistic is sampled.
        """
        if session == None:
            session = self.session
        deadline = time.time() + timeout
        while True:
            with wttest.open_cursor(session, 'statistics:') as stat_cursor:
                state = stat_cursor[stat.conn.checkpoint_state][2]
            if state != 0:
                break
            self.assertLess(time.time(), deadline,
                'checkpoint did not start running within %d seconds' % timeout)
            time.sleep(poll_interval)
