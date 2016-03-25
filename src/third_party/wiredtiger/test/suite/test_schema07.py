#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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

# test_schema07.py
#    Test that long-running tests don't fill the cache with metadata
class test_schema07(wttest.WiredTigerTestCase):
    tablename = 'table:test_schema07'

    def conn_config(self, dir):
        return 'cache_size=10MB'

    @wttest.longtest("Creating many tables shouldn't fill the cache")
    def test_many_tables(self):
        s = self.session
        # We have a 10MB cache, metadata is (well) over 512B per table,
        # if we can create 20K tables, something must be cleaning up.
        for i in xrange(20000):
            uri = '%s-%06d' % (self.tablename, i)
            s.create(uri)
            c = s.open_cursor(uri)
            # This will block if the metadata fills the cache
            c["key"] = "value"
            c.close()
            self.session.drop(uri)

if __name__ == '__main__':
    wttest.run()
