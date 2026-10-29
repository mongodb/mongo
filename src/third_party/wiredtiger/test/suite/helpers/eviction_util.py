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

# eviction_util.py
# Shared base class used by eviction tests.
class eviction_util(wttest.WiredTigerTestCase):

    def scrub_images(self):
        """The (pages, bytes) gauges of the images checkpoint scrub currently retains."""
        return (self.get_stat(stat.conn.cache_scrub_image_pages),
          self.get_stat(stat.conn.cache_scrub_image_bytes))

    def wait_for_scrub_images_released(self, timeout=30):
        """Wait until no retained scrub image is accounted for anywhere in the connection.

        Freeing the pages that hold the images is not always finished by the time the call that
        triggers it returns: dropping a clean tree marks its handle dead and leaves the discard to
        the sweep server. Requiring both gauges to reach zero is also what checks that they drain
        together rather than one outrunning the other. Callers wanting this to resolve promptly
        rather than on the default scan interval should shorten file_manager.close_scan_interval."""
        deadline = time.time() + timeout
        while True:
            pages, nbytes = self.scrub_images()
            if (pages, nbytes) == (0, 0):
                return
            self.assertLess(time.time(), deadline,
                'retained scrub images were not released within {}s: '
                'pages={}, bytes={}'.format(timeout, pages, nbytes))
            time.sleep(0.1)

    def evict_cursor_tw_cleanup(self, uri, nrows):
        # Configure debug behavior at the session level to evict the page when released.
        # This is necessary when willing to trigger the time window cleanup code as application
        # threads are not allowed to execute this code.
        session_evict = self.conn.open_session("debug=(release_evict_page=true)")
        session_evict.begin_transaction("ignore_prepare=true")
        evict_cursor = session_evict.open_cursor(uri, None, None)
        for i in range (nrows):
            evict_cursor.set_key(i)
            evict_cursor.search()
            if i % 10 == 0:
                evict_cursor.reset()
        evict_cursor.close()
        session_evict.rollback_transaction()
        session_evict.close()

    def populate(self, uri, start_key, num_keys, value):
        c = self.session.open_cursor(uri, None)
        for k in range(start_key, num_keys):
            self.session.begin_transaction()
            c[k] = value
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(k + 1))
        c.close()
