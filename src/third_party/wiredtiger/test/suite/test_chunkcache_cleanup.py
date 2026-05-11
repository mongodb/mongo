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

# Test startup cleanup on reopen.
class test_chunkcache_cleanup(wttest.WiredTigerTestCase):
    uri = 'file:WiredTigerCC.wt'

    def metadata_search(self):
        meta = self.session.open_cursor('metadata:', None, None)
        meta.set_key(self.uri)
        ret = meta.search()
        meta.close()
        return ret

    def test_startup_cleanup_runs_on_reopen(self):
        # Seed the chunk cache metafile so reopen has cleanup work to do.
        self.session.create(self.uri, 'key_format=u,value_format=u')
        self.assertEqual(self.metadata_search(), 0)
        self.reopen_conn()

        # Confirm that startup cleanup removed the chunk cache metadata entry.
        self.assertEqual(self.metadata_search(), wiredtiger.WT_NOTFOUND)
