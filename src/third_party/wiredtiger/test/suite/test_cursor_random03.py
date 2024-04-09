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
from wtdataset import SimpleDataSet

# test_cursor_random03.py
#    This python test reproduces the WT-12225 bug where the same stream of numbers are generated
# from the random cursor. In the presence of where all the values are the insert list, open up
# two cursors close to each other in time, and both cursors will return back the exact same stream
# of records again. The test makes sure that records returned in both cursors return at least
# one record that is different, ensuring that the stream of numbers returned from __wt_random
# are not in the same pattern.
class test_cursor_random03(wttest.WiredTigerTestCase):
    def test_cursor_random_bug(self):
        uri = 'table:random'

        # Do not change the chosen number of records. The records is to make sure that
        # the skip insert list random estimate is a power of 2 to act as a mask.
        # Also set the leaf-page-max value, otherwise the page might split.
        ds = SimpleDataSet(self, uri, 2135, config='leaf_page_max=100MB')
        ds.populate()

        for _ in range(0, 5000):
            random_keys = []

            cursor = self.session.open_cursor(uri, None, 'next_random=true')

            # Perform 100 nexts and append to our random keys.
            for _ in range(0, 100):
                self.assertEqual(cursor.next(), 0)
                current = cursor.get_key()
                random_keys.append(current)
            cursor.close()

            # The random cursor initial seed is time based, reset the initial random seed again
            # to make sure that we have not generated with the same random numbers
            cursor = self.session.open_cursor(uri, None, 'next_random=true')
            found_different = False
            for i in range(0, 100):
                self.assertEqual(cursor.next(), 0)
                current = cursor.get_key()
                # Exit early once we found a key that is different.
                if (random_keys[i] != current):
                    found_different = True
                    break
            cursor.close()

            # We expect that the values between two recently opened cursors return different stream
            # of records.
            self.assertTrue(found_different)

