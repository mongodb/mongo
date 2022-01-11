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

# test_bug013.py
#    Test data consistency in LSM with updates. Ensure that overwrite
#    cursors see all entries in the tree (i.e: they open cursors on all
#    chunks in the LSM tree).
#    See JIRA BF-829
class test_bug013(wttest.WiredTigerTestCase):
    """
    Test LSM data consistency.
    """
    uri = 'table:test_bug013'

    def check_entries(self, keys):
        # Test by iterating.
        cursor = self.session.open_cursor(self.uri, None, None)
        i = 0
        for i1, i2, i3, v1 in cursor:
            self.assertEqual( keys[i], [i1, i2, i3])
            i += 1
        cursor.close()
        self.assertEqual(i, len(keys))

    def test_lsm_consistency(self):
        self.session.create(self.uri, 'key_format=iii,value_format=i,type=lsm')
        cursor = self.session.open_cursor(self.uri, None, None)
        cursor[(2, 6, 1)] = 0
        cursor.close()
        # Ensure the first chunk is flushed to disk, so the tree will have
        # at least two chunks. Wrapped in a try, since it sometimes gets
        # an EBUSY return
        try:
            self.session.verify(self.uri, None)
        except wiredtiger.WiredTigerError:
            pass

        # Add a key
        cursor = self.session.open_cursor(self.uri, None, 'overwrite=false')
        cursor[(1, 5, 1)] = 0
        cursor.close()

        # Remove the key we just added. If the LSM code is broken, the
        # search for the key we just inserted returns not found - so the
        # key isn't actually removed.
        cursor = self.session.open_cursor(self.uri, None, 'overwrite=false')
        cursor.set_key((1, 5, 1))
        cursor.remove()
        cursor.close()

        # Verify that the data is as we expect
        self.check_entries([[2, 6, 1]])

if __name__ == '__main__':
    wttest.run()
