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

from contextlib import closing
import wttest
from wiredtiger import stat
from wtscenario import make_scenarios


class test_eviction09(wttest.WiredTigerTestCase):
    """Debug release eviction evicts ordinary pages but never cache-resident pages."""

    uri = f"file:{__qualname__}"

    release_evict_modes = [
        ("cursor_evict", dict(session_cfg="", cursor_cfg="debug=(release_evict)")),
        ("session_evict", dict(session_cfg="debug=(release_evict_page=true)", cursor_cfg="")),
    ]
    scenarios = make_scenarios(release_evict_modes)

    def evicted_pages(self):
        """Return the total number of clean and dirty pages evicted from the file."""
        clean = self.get_stat(stat.dsrc.cache_eviction_clean, self.uri)
        dirty = self.get_stat(stat.dsrc.cache_eviction_dirty, self.uri)
        return clean + dirty

    def pages_evicted_on_release(self, file_config):
        """Read a row from a new file with release eviction and return the pages evicted."""
        self.session.create(self.uri, "key_format=i,value_format=S," + file_config)
        with wttest.open_cursor(self.session, self.uri) as cursor:
            cursor[1] = "value"

        evicted = self.evicted_pages()

        with (
            closing(self.conn.open_session(self.session_cfg)) as session,
            wttest.open_cursor(session, self.uri, config=self.cursor_cfg) as cursor,
        ):
            self.assertEqual(cursor[1], "value")

        return self.evicted_pages() - evicted

    def test_release_evict_ordinary(self):
        """Release eviction evicts pages from an ordinary file."""
        self.assertGreater(self.pages_evicted_on_release("cache_resident=false"), 0)

    def test_release_evict_cache_resident(self):
        """Release eviction never evicts pages from a cache-resident file."""
        self.assertEqual(self.pages_evicted_on_release("cache_resident=true"), 0)


if __name__ == "__main__":
    wttest.run()
