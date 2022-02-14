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
# compression
# [END_TAGS]
#
# test_dictionary.py
#       Smoke test dictionary compression.

from wtscenario import make_scenarios
from wtdataset import simple_key
from wiredtiger import stat
import wttest

# Smoke test dictionary compression.
class test_dictionary(wttest.WiredTigerTestCase):
    conn_config = 'statistics=(all)'
    scenarios = make_scenarios([
        ('row', dict(key_format='S')),
        ('var', dict(key_format='r')),
    ])

    # Smoke test dictionary compression.
    def test_dictionary(self):
        nentries = 25000
        uri = 'file:test_dictionary'    # This is a btree layer test.

        # Create the object, open the cursor, insert some records with identical values. Use
        # a reasonably large page size so most of the items fit on a page. Use alternating
        # values, otherwise column-store will RLE compress them into a single item.
        config='leaf_page_max=64K,dictionary=100,value_format=S,key_format='
        self.session.create(uri, config + self.key_format)
        cursor = self.session.open_cursor(uri, None)
        i = 0
        while i < nentries:
            i = i + 1
            cursor[simple_key(cursor, i)] = "the same value as the odd items"
            i = i + 1
            cursor[simple_key(cursor, i)] = "the same value as the even items"
        cursor.close()

        # Checkpoint to force the pages through reconciliation.
        self.session.checkpoint()

        # Confirm the dictionary was effective.
        cursor = self.session.open_cursor('statistics:' + uri, None, None)
        self.assertGreater(cursor[stat.dsrc.rec_dictionary][2], nentries - 100)

if __name__ == '__main__':
    wttest.run()
