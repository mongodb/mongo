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

# compact_util.py
# Shared base class used by compact tests.
class compact_util(wttest.WiredTigerTestCase):

    def delete_range(self, uri, num_keys):
        c = self.session.open_cursor(uri, None)
        for i in range(num_keys):
            c.set_key(i)
            c.remove()
        c.close()

    def get_bg_compaction_running(self):
        return self.get_stat(stat.conn.background_compact_running)

    def get_bg_compaction_success(self):
        return self.get_stat(stat.conn.background_compact_success)

    def get_bytes_recovered(self):
        return self.get_stat(stat.conn.background_compact_bytes_recovered)

    def get_bytes_avail_for_reuse(self, uri):
        return self.get_stat(stat.dsrc.block_reuse_bytes, uri)

    # Return stats that track the progress of compaction for a given file.
    def get_compact_progress_stats(self, uri):
        cstat = self.session.open_cursor('statistics:' + uri, None, 'statistics=(all)')
        statDict = {}
        statDict["bytes_rewritten_expected"] = cstat[stat.dsrc.btree_compact_bytes_rewritten_expected][2]
        statDict["pages_reviewed"] = cstat[stat.dsrc.btree_compact_pages_reviewed][2]
        statDict["pages_rewritten"] = cstat[stat.dsrc.btree_compact_pages_rewritten][2]
        statDict["pages_rewritten_expected"] = cstat[stat.dsrc.btree_compact_pages_rewritten_expected][2]
        statDict["pages_skipped"] = cstat[stat.dsrc.btree_compact_pages_skipped][2]
        cstat.close()
        return statDict

    def get_files_compacted(self, uris):
        files_compacted = 0
        for uri in uris:
            if self.get_pages_rewritten(uri) > 0:
                files_compacted += 1
        return files_compacted

    def get_pages_rewritten(self, uri):
        return self.get_stat(stat.dsrc.btree_compact_pages_rewritten, uri)

    def get_bg_compaction_files_skipped(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        skipped = stat_cursor[stat.conn.background_compact_skipped][2]
        stat_cursor.close()
        return skipped

    # Return the size of the given file.
    def get_size(self, uri):
        # To allow this to work on systems without ftruncate,
        # get the portion of the file allocated, via 'statistics=(all)',
        # not the physical file size, via 'statistics=(size)'.
        cstat = self.session.open_cursor('statistics:' + uri, None, 'statistics=(all)')
        sz = cstat[stat.dsrc.block_size][2]
        cstat.close()
        return sz

    def get_stat(self, stat, uri = None):
        if not uri:
            uri = ''
        stat_cursor = self.session.open_cursor(f'statistics:{uri}', None, None)
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def populate(self, uri, start_key, num_keys, value=None, value_size=1024):
        c = self.session.open_cursor(uri, None)
        for k in range(start_key, num_keys):
            if not value:
                c[k] = ('%07d' % k) + '_' + 'a' * (value_size - 2)
            else:
                c[k] = value
        c.close()

    def turn_on_bg_compact(self, config = ''):
        self.session.compact(None, f'background=true,{config}')
        while not self.get_bg_compaction_running():
            time.sleep(0.1)

    def turn_off_bg_compact(self):
        self.session.compact(None, 'background=false')
        while self.get_bg_compaction_running():
            time.sleep(0.1)

    def truncate(self, uri, start_key, end_key):
        lo_cursor = self.session.open_cursor(uri)
        hi_cursor = self.session.open_cursor(uri)
        lo_cursor.set_key(start_key)
        hi_cursor.set_key(end_key)
        self.assertEqual(self.session.truncate(None, lo_cursor, hi_cursor, None), 0)
        lo_cursor.close()
        hi_cursor.close()
