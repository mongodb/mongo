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
    configuration_items = ['exclude=["table:a.wt"]', 'free_space_target=10MB', 'timeout=60']

    def get_bg_compaction_files_skipped(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        skipped = stat_cursor[stat.conn.background_compact_skipped][2]
        stat_cursor.close()
        return skipped

    def get_bg_compaction_running(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        compact_running = stat_cursor[stat.conn.background_compact_running][2]
        stat_cursor.close()
        return compact_running

    def get_bg_compaction_success(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        skipped = stat_cursor[stat.conn.background_compact_success][2]
        stat_cursor.close()
        return skipped

    def turn_off_bg_compact(self):
        self.session.compact(None, 'background=false')
        while self.get_bg_compaction_running():
            time.sleep(1)

    def turn_on_bg_compact(self, config=''):
        self.session.compact(None, f'background=true,{config}')
        while not self.get_bg_compaction_running():
            time.sleep(1)

    def test_background_compact_api(self):
        # FIXME-WT-11399
        if self.runningHook('tiered'):
            self.skipTest("Compaction isn't supported on tiered tables")

        # We cannot trigger the background compaction on a specific API. Note that the URI is
        # not relevant here, the corresponding table does not need to exist for this check.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.compact("file:123", 'background=true'),
            '/Background compaction does not work on specific URIs/')

        # We cannot set other configurations while turning off the background server.
        for item in self.configuration_items:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.compact(None, f'background=false,{item}'),
                '/configuration cannot be set when disabling the background compaction server/')

        # We cannot exclude invalid URIs when enabling background compaction.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
            self.session.compact(None, 'background=true,exclude=["file:a"]'),
            '/can only exclude objects of type "table"/')

        # Enable the background compaction server.
        self.turn_on_bg_compact()

        # We cannot reconfigure the background server.
        for item in self.configuration_items:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.compact(None, f'background=true,{item}'),
                '/Cannot reconfigure background compaction while it\'s already running/')

        # Wait for background compaction to process the HS.
        while self.get_bg_compaction_success() == 0:
            time.sleep(1)

        # Disable the background compaction server.
        self.turn_off_bg_compact()

        # Background compaction should have tried to compact the HS hence skipped no files.
        assert self.get_bg_compaction_files_skipped() == 0
        assert self.get_bg_compaction_success() == 1

        # Enable background and configure it to run once. Don't use the helper function as the
        # server may go to sleep before we have the time to check it is actually running.
        self.session.compact(None, 'background=true,run_once=true')

        # Wait for background compaction to process the HS table. Even though there is no work to
        # do, it is considered as a success.
        while self.get_bg_compaction_success() == 1:
            time.sleep(1)

        # Ensure background compaction stops by itself.
        while self.get_bg_compaction_running():
            time.sleep(1)

        # When running once, background compaction should not skip files.
        assert self.get_bg_compaction_files_skipped() == 0
        assert self.get_bg_compaction_success() == 2

        # Enable the server again but with default options, the HS should be skipped.
        self.turn_on_bg_compact()

        while self.get_bg_compaction_files_skipped() != 1:
            time.sleep(1)

        self.turn_off_bg_compact()

        # Background compaction may have been inspecting a table when disabled, which is considered
        # as an interruption, ignore that message.
        self.ignoreStdoutPatternIfExists('background compact interrupted by application')
