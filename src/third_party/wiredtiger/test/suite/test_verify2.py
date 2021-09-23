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
class test_verify2(wttest.WiredTigerTestCase):
    tablename = 'test_verify'
    params = 'key_format=S,value_format=S'
    uri = 'table:' + tablename

    # Create an empty table and insert content.
    # The first call to verify is expected to return to EBUSY due to the dirty content. Call
    # checkpoint to make the table clean, the next verify call should succeed.
    def test_verify_ckpt(self):
        self.assertEqual(self.session.create(self.uri, self.params), 0)
        self.assertEqual(self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10)), 0)

        # Insert data.
        cursor = self.session.open_cursor(self.uri)
        cursor["0"] = "000"
        cursor.close()

        # Calling verify without checkpointing before will return EBUSY because of the dirty data.
        self.assertTrue(self.raisesBusy(lambda: self.session.verify(self.uri, None)),
                        "was expecting API call to fail with EBUSY")

        # Checkpointing will get rid of the dirty data.
        self.assertEqual(self.session.checkpoint(), 0)

        # Verify.
        self.assertEqual(self.session.verify(self.uri, None), 0)

    # Create an empty table and search a key. This used to mark the associated btree as dirty. In
    # fact, because the tree is empty, its only reference to a leaf page is marked as deleted and we
    # instantiate the deleted page in this case. Before WT-8126, this would mark the btree as
    # modified.
    def test_verify_search(self):
        self.assertEqual(self.session.create(self.uri, self.params), 0)
        self.assertEqual(self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(10)), 0)

        # Search for data.
        cursor = self.session.open_cursor(self.uri)
        cursor.set_key("1")
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        cursor.close()

        # We don't need to call checkpoint before calling verify as the btree is not marked as
        # modified.
        self.assertEqual(self.session.verify(self.uri, None), 0)

if __name__ == '__main__':
    wttest.run()
