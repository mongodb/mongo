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

import wiredtiger, wttest

# test_compact08.py
# Verify compaction for in-memory and readonly databases is not allowed.
class test_compact08(wttest.WiredTigerTestCase):
    uri = 'file:test_compact08'

    def start_background_compaction(self):
        self.session.compact(None, 'background=true')

    def start_foreground_compaction(self):
        self.session.compact(self.uri, None)

    def test_compact08(self):

        # Create a table.
        self.session.create(self.uri, None)

        # Create an inmemory database.
        self.reopen_conn(config='in_memory=true')

        # Foreground compaction.
        with self.expectedStdoutPattern('Compact does not work for in-memory databases'):
           self.start_foreground_compaction()

        # Background compaction.
        with self.expectedStdoutPattern('Background compact cannot be configured for in-memory or readonly databases'):
            self.assertRaisesException(wiredtiger.WiredTigerError,
                lambda: self.start_background_compaction())

        # Reopen in readonly mode.
        self.reopen_conn(config='in_memory=false,readonly=true')

        # Foreground compaction.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.start_foreground_compaction(),
            '/Operation not supported/')

        # Background compaction.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.start_foreground_compaction(),
            '/Operation not supported/')
