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
#
# test_bug004.py
#       Regression tests.

import wiredtiger, wttest
from helper import key_populate, value_populate

# Check to make sure we see the right versions of overflow keys and values
# when they are deleted in reconciliation without having been instantiated
# in the system.
class test_bug004(wttest.WiredTigerTestCase):
    # This is a btree layer test, test files, ignore tables.
    uri = 'file:test_ovfl_key'

    # Use a small page size because we want to create overflow items
    config = 'allocation_size=512,' +\
        'leaf_page_max=512,value_format=S,key_format=S'

    nentries = 30

    def test_bug004(self):
        # Create the object, fill with overflow keys and values.
        self.session.create(self.uri, self.config)
        c1 = self.session.open_cursor(self.uri, None)
        for i in range(1, self.nentries):
            c1[key_populate(c1, i) + 'abcdef' * 100] = \
                value_populate(c1, i) + 'abcdef' * 100
        c1.close()

        # Verify the object, force it to disk, and verify the on-disk version.
        self.session.verify(self.uri)
        self.reopen_conn()
        self.session.verify(self.uri)

        # Create a new session and start a transaction to force the engine
        # to access old versions of the key/value pairs.
        tmp_session = self.conn.open_session(None)
        tmp_session.begin_transaction("isolation=snapshot")

        # Load the object and use truncate to delete a set of records.  (I'm
        # using truncate because it doesn't instantiate keys, all other ops
        # currently do -- that's unlikely to change, but is a problem for the
        # test going forward.)
        c1 = self.session.open_cursor(self.uri, None)
        c1.set_key(key_populate(c1, self.nentries - 5) + 'abcdef' * 100)
        c2 = self.session.open_cursor(self.uri, None)
        c2.set_key(key_populate(c2, self.nentries + 5) + 'abcdef' * 100)
        self.session.truncate(None, c1, c2, None)
        c1.close()
        c2.close()

        # Checkpoint, freeing overflow blocks.
        self.session.checkpoint()

        # Use the snapshot cursor to retrieve the old key/value pairs
        c1 = tmp_session.open_cursor(self.uri, None)
        c1.set_key(key_populate(c1, 1) + 'abcdef' * 100)
        c1.search()
        for i in range(2, self.nentries):
            c1.next()
            self.assertEquals(
                c1.get_key(), key_populate(c1, i) + 'abcdef' * 100)
            self.assertEquals(
                c1.get_value(), value_populate(c1, i) + 'abcdef' * 100)


if __name__ == '__main__':
    wttest.run()
