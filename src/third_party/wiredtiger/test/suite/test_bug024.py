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
# [TEST_TAGS]
# connection_api:turtle_file
# [END_TAGS]

import wttest
from wtdataset import SimpleDataSet
import os, shutil

# test_bug024.py
# WT-6526: test that we can successfully open a readonly connection after it was stopped while
# the temporary turtle file existed. We simulate that by copying the turtle file to its temporary name
# and then opening the connection readonly.
class test_bug024(wttest.WiredTigerTestCase):
    conn_config = ('cache_size=50MB')

    # Create a table.
    uri = "table:test_bug024"

    def test_bug024(self):
        nrows = 10
        ds = SimpleDataSet(self, self.uri, nrows, key_format="S", value_format='u')
        ds.populate()

        self.conn.close()
        # Copying the file manually to recreate the issue described in WT-6526.
        shutil.copy('WiredTiger.turtle', 'WiredTiger.turtle.set')

        # Open wiredtiger in new directory and in readonly mode.
        conn = self.wiredtiger_open(self.home, "readonly")
        conn.close()

if __name__ == '__main__':
    wttest.run()
