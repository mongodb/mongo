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
import wiredtiger
import os
import errno

# test_ovfl01
# Test bulk insert does not leave orphaned overflow keys.
class test_ovfl01(wttest.WiredTigerTestCase):
    # Connection configurations:
    #     - Use strings as the key format to allow for very large key sizes.
    #     - Set a very small leaf_key_max. Keys larger than this will be stored as overflow keys.
    #.    - Set a small leaf_page_max to create more leaf pages and page splits.
    table_config = 'key_format=S,value_format=S,leaf_key_max=10B,leaf_value_max=10B,leaf_page_max=4KB'
    conn_config = 'cache_size=100MB,statistics=(all),timing_stress_for_test=(failpoint_rec_split_write)'
    uri = 'table:test_ovfl01'

    num_keys = 10 * 1000

    def populate(self, uri):
        c = self.session.open_cursor(uri, None, 'bulk')
        for k in range(0, self.num_keys):
            key = str(k).zfill(10) + '_' + 'k' * 1024
            value = str(k).zfill(10) + '_' + 'v' * 1024
            c.set_key(key)
            c.set_value(value)

            try:
                c.insert()
            except wiredtiger.WiredTigerError as e:
                self.pr(f'insert error: {str(e)}\non key: {key}')
                # The EBUSY error means we have hit the fail point.
                if str(e) != os.strerror(errno.EBUSY):
                    raise e

        # Closing the cursor might hit the failpoint if the page needs to split during
        # reconciliation, try again if this occurs.
        while True:
            try:
                c.close()
                return
            except wiredtiger.WiredTigerError as e:
                if str(e) != os.strerror(errno.EBUSY):
                    raise e

    def test_ovfl01(self):
        # Create and populate a table.
        self.session.create(self.uri, self.table_config)
        self.populate(self.uri)

        # Write to disk.
        self.session.checkpoint()

        # Verify on disk contents.
        self.session.verify(self.uri)

        self.ignoreStdoutPatternIfExists('bulk insert failed during page split')
