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
from wtdataset import SimpleDataSet

# test_flcs05.py
#
# Test the read-back-value-on-failure path of insert.

class test_flcs05(wttest.WiredTigerTestCase):
    conn_config = 'in_memory=false'

    # Read a value by failing insert.
    def tryread(self, cursor, k, v):
        cursor.set_key(k)
        # Set something other than the expected value to increase the chances that
        # we fail if the value just doesn't get set.
        cursor.set_value(v + 1)
        self.assertRaisesHavingMessage(
            wiredtiger.WiredTigerError, lambda: cursor.insert(), '/WT_DUPLICATE_KEY/')
        self.assertEqual(cursor.get_value(), v)

    # Evict the page to force reconciliation.
    def evict(self, ds, key, check_value):
        evict_cursor = ds.open_cursor(ds.uri, None, "debug=(release_evict)")
        self.session.begin_transaction()
        v = evict_cursor[key]
        self.assertEqual(v, check_value)
        self.assertEqual(evict_cursor.reset(), 0)
        self.session.rollback_transaction()
        evict_cursor.close()

    @wttest.skip_for_hook("timestamp", "fails at begin_transaction")  # FIXME-WT-9809
    def test_flcs(self):
        uri = "table:test_flcs05"
        nrows = 44
        ds = SimpleDataSet(
            self, uri, nrows, key_format='r', value_format='6t', config='leaf_page_max=4096')
        ds.populate()

        updatekey1 = 33
        updatekey2 = 37
        appendkey1 = nrows + 10

        cursor = ds.open_cursor(uri, None, 'overwrite=false')

        # Write a few records.
        #self.session.begin_transaction()
        #for i in range(1, nrows + 1):
        #    self.prout("foo {}".format(i))
        #    cursor.set_key(i)
        #    cursor.set_value(i)
        #    self.assertEqual(cursor.update(), 0)
        #self.session.commit_transaction()

        # There are five cases:
        #    1. A nonzero value.
        #    2. A zero/deleted value that's been reconciled.
        #    3. A zero/deleted value that hasn't been reconciled but that's on/over a page.
        #    4. A zero/deleted value that hasn't been reconciled and is in the append list.
        #    5. A value that only exists implicitly becaues the append list has gone past it.

        # Nonzero value.
        self.tryread(cursor, updatekey1, ds.value(updatekey1))

        # Deleted value that hasn't been reconciled.
        cursor.set_key(updatekey2)
        self.assertEqual(cursor.remove(), 0)
        self.tryread(cursor, updatekey2, 0)

        # Deleted value that has been reconciled.
        self.evict(ds, updatekey2, 0)
        self.tryread(cursor, updatekey2, 0)

        # Deleted value in the append list.
        cursor[appendkey1] = appendkey1
        cursor.set_key(appendkey1)
        self.assertEqual(cursor.remove(), 0)
        self.tryread(cursor, appendkey1, 0)

        # Implicit value.
        self.tryread(cursor, appendkey1 - 1, 0)
