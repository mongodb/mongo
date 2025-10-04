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

import wttest

# test_drop01.py
# Test WT_SESSION->drop should clean up history store.
class test_drop01(wttest.WiredTigerTestCase):
    def add_timestamp_data(self, uri, key, val1, val2, timestamp):
        self.session.begin_transaction()
        cursor = self.session.open_cursor(uri, None, None)
        cursor[key] = (val1, val2)
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp))

    def get_hs_size(self):
        cursor = self.session.open_cursor("file:WiredTigerHS.wt")
        size = 0
        while cursor.next() == 0:
            size += 1
        cursor.close()
        return size

    uri = 'table:test_drop01'
    name = 'test_drop01'
    def test_drop_hs_truncate(self):
        # Create the table with two column groups.
        self.session.create(self.uri, "key_format=S,value_format=SS,"
                    "columns=(key,value1,value2),colgroups=(cg1,cg2)")
        self.session.create("colgroup:" + self.name + ":cg1", "columns=(value1)")
        self.session.create("colgroup:" + self.name + ":cg2", "columns=(value2)")

        # Insert a record, then update it.
        self.add_timestamp_data(self.uri, "keyA", "v1t5", "v2t5", 5)
        self.add_timestamp_data(self.uri, "keyA", "v1t8", "v2t8", 8)
        self.session.checkpoint()

        # We should expect two records in history store each for cg1 and cg2.
        self.assertEqual(self.get_hs_size(), 2)

        self.session.drop(self.uri)

        # Drop should clean up history store for that table, there'll be no record in history store.
        self.assertEqual(self.get_hs_size(), 0)

if __name__ == '__main__':
    wttest.run()
