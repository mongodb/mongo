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
#
# test_hazard.py
#       Hazard pointer tests.

import wttest
from wtdataset import SimpleDataSet

# Regression tests.
class test_hazard(wttest.WiredTigerTestCase):

    # Allocate a large number of hazard pointers in a session, forcing the
    # hazard pointer array to repeatedly grow.
    def test_hazard(self):
        uri = "table:hazard"
        ds = SimpleDataSet(self, uri, 1000)
        ds.populate()

        # Open 10,000 cursors and pin a page to set a hazard pointer.
        cursors = []
        for i in range(0, 10000):
            c = self.session.open_cursor(uri, None)
            c.set_key(ds.key(10))
            c.search()
            cursors.append(c)

        # Close the cursors, clearing the hazard pointer.
        for c in cursors:
            c.close()

if __name__ == '__main__':
    wttest.run()
