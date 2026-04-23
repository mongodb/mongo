#!/usr/bin/env python3
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

# Test that deprecated chunk cache configuration is handled correctly.
class test_chunkcache07(wttest.WiredTigerTestCase):

    def conn_config(self):
        return ''

    # Test that opening with chunk_cache enabled fails with an appropriate error.
    def test_chunk_cache_enabled_error(self):
        self.close_conn()

        config = 'create,chunk_cache=(enabled=true)'
        msg = '/chunk cache has been deprecated and is no longer supported/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('.', config), msg)

        # Reopen normally so tearDown can close cleanly.
        self.open_conn()

    # Test that opening with chunk_cache disabled issues a deprecation warning.
    def test_chunk_cache_disabled_warning(self):
        self.close_conn()

        config = 'create,chunk_cache=(enabled=false)'
        with self.expectedStdoutPattern('chunk cache has been deprecated and is no longer supported'):
            self.open_conn('.', config)
