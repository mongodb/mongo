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

# Base class providing dhandle-sweep helpers for any test that needs them.
class sweep_util(wttest.WiredTigerTestCase):

    def wait_for_sweep(self, baseline=None, increment=1, statistic=stat.conn.dh_sweeps,
                       session=None, timeout=60, poll_interval=0.5):
        """
        Poll until a sweep statistic has advanced by at least `increment` over `baseline`. If
        `baseline` is None it is sampled at entry. Bounded so the test fails fast with a clear
        message instead of spinning to a task-level timeout if the sweep server makes no progress.
        poll_interval controls how often the statistic is sampled. Returns the observed value.
        """
        if session is None:
            session = self.session
        if baseline is None:
            with wttest.open_cursor(session, 'statistics:') as stat_cursor:
                baseline = stat_cursor[statistic][2]
        deadline = time.time() + timeout
        while True:
            with wttest.open_cursor(session, 'statistics:') as stat_cursor:
                value = stat_cursor[statistic][2]
            if value - baseline >= increment:
                return value
            self.assertLess(time.time(), deadline,
                'sweep statistic did not advance by %d within %d seconds' % (increment, timeout))
            time.sleep(poll_interval)
