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
import wiredtiger, wttest
from wiredtiger import stat

# test_compact06.py
# Test background compaction API usage.
class test_compact06(wttest.WiredTigerTestCase):
    def get_bg_compaction_running(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        compact_running = stat_cursor[stat.conn.background_compact_running][2]
        stat_cursor.close()
        return compact_running
    
    def test_background_compact_api(self):
        #   1. We cannot trigger the background compaction on a specific API. Note that the URI is
        # not relevant here, the corresponding table does not need to exist for this check.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.compact("file:123", 'background=true'), '/Background compaction does not work on specific URIs/')
            
        #   2. We cannot set other configurations while turning off the background server.
        items = ['exclude=["table:a.wt"]', 'free_space_target=10MB', 'timeout=60']
        for item in items:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.compact(None, f'background=false,{item}'),
                '/configuration cannot be set when disabling the background compaction server/')

        #   3. We cannot exclude invalid URIs when enabling background compaction.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.compact(None, 'background=true,exclude=["file:a"]'), '/can only exclude objects of type "table"/')

        #   4. Enable the background compaction server.
        self.session.compact(None, 'background=true')

        # Wait for the background server to wake up.
        compact_running = self.get_bg_compaction_running()
        while not compact_running:
            time.sleep(1)
            compact_running = self.get_bg_compaction_running()
        self.assertEqual(compact_running, 1)

        #   5. We cannot reconfigure the background server.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.compact(None, 'background=true,free_space_target=10MB'), '/Cannot reconfigure background compaction while it\'s already running/')

        #   6. Disable the background compaction server.
        self.session.compact(None, 'background=false')

if __name__ == '__main__':
    wttest.run()
