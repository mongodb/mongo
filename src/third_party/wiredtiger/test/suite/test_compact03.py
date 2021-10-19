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
# test_compact03.py
#   Test that compact doesn't reduce the file size when there are overflow values at the
#   end of file.
#

import time, wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# Test compact behaviour with overflow values.
class test_compact03(wttest.WiredTigerTestCase):

    uri='table:test_compact03'

    fileConfig = [
        ('1KB', dict(fileConfig='allocation_size=1KB,leaf_page_max=1KB')),
        ('4KB', dict(fileConfig='allocation_size=4KB,leaf_page_max=4KB')),
    ]

    useTruncate = [
        ('no_truncate', dict(truncate=False)),
        ('truncate', dict(truncate=True))
    ]
    scenarios = make_scenarios(fileConfig, useTruncate)

    # Enable stats and use a cache size that can fit table in the memory.
    conn_config = 'statistics=(all),cache_size=100MB'

    # We want to test how compaction interacts with overflow values. Test flow is as follows:
    #
    # 1. Populate a table with relatively small page size.
    # 2. Checkpoint and get stats on the table to confirm the size.
    # 3. Add few thousand overflow values. It is expected that these will be written at the end of
    #    file.
    # 4. Perform checkpoint to ensure overflow values are written on disk.
    # 5. Delete middle ~90% of the normal values in the table.
    # 6. Perform checkpoint so compact can find something to work with.
    # 7. Call compact.
    # 8. Get stats on compacted table expecting that there will no change in size given we have
    #    overflow keys at the end of file.
    # 9. Insert some normal values again. They will be written in the free extents in the middle
    #    of the file. Therefore, expect no increase in file size.
    #
    # We want to have around 20000 leaf pages. With minimum 1KB page allocation size, the table
    # is expected to have at least 25 MByte worth of data. We can then experiment with deleting
    # range of keys in middle to test how comapction works.

    normalValue = "abcde" * 10
    overflowValue = "abcde" * 1000
    nrecords = 400000 # To create ~25 MB table
    expectedTableSize = 20 # Mbytes
    nOverflowRecords = 5000

    # Return stats that track the progress of compaction.
    def getCompactProgressStats(self):
        cstat = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(all)')
        statDict = {}
        statDict["pages_reviewed"] = cstat[stat.dsrc.btree_compact_pages_reviewed][2]
        statDict["pages_skipped"] = cstat[stat.dsrc.btree_compact_pages_skipped][2]
        statDict["pages_selected"] = cstat[stat.dsrc.btree_compact_pages_write_selected][2]
        statDict["pages_rewritten"] = cstat[stat.dsrc.btree_compact_pages_rewritten][2]
        cstat.close()
        return statDict

    # Return the size of the file
    def getSize(self):
        # To allow this to work on systems without ftruncate,
        # get the portion of the file allocated, via 'statistics=(all)',
        # not the physical file size, via 'statistics=(size)'.
        cstat = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(all)')
        sz = cstat[stat.dsrc.block_size][2]
        cstat.close()
        return sz

    # Create a table, add keys with both big and small values.
    def test_compact03(self):

        mb = 1024 * 1024
        # 1. Create a table with relatively small page size.
        params = 'key_format=i,value_format=S,' + self.fileConfig
        self.session.create(self.uri, params)
        c = self.session.open_cursor(self.uri, None)
        for i in range(self.nrecords):
            c[i] = self.normalValue
        c.close()

        # 2. Checkpoint and get stats on the table to confirm the size.
        self.session.checkpoint()
        sizeWithoutOverflow = self.getSize()
        self.pr('After populate ' + str(sizeWithoutOverflow // mb) + 'MB')
        self.assertGreater(sizeWithoutOverflow, self.expectedTableSize * mb)

        # 3. Add overflow values.
        c = self.session.open_cursor(self.uri, None)
        for i in range(self.nOverflowRecords):
            c[i + self.nrecords] = self.overflowValue
        c.close()

        # 4. Perform checkpoint to ensure overflow values are written to the disk.
        self.session.checkpoint()
        sizeWithOverflow = self.getSize()
        self.pr('After inserting overflow values ' + str(sizeWithoutOverflow // mb) + 'MB')
        self.assertGreater(sizeWithOverflow, sizeWithoutOverflow)

        # 5. Delete middle ~90% of the normal values in the table.
        if self.truncate:
            c1 = self.session.open_cursor(self.uri, None)
            c2 = self.session.open_cursor(self.uri, None)
            c1.set_key((self.nrecords // 100) * 5)
            c2.set_key((self.nrecords // 100) * 95)
            self.assertEqual(self.session.truncate(None, c1, c2, None), 0)
            c1.close()
            c2.close()
        else:
            c = self.session.open_cursor(self.uri, None)
            for i in range((self.nrecords // 100) * 5, (self.nrecords // 100) * 95):
                c.set_key(i)
                self.assertEqual(c.remove(), 0)
            c.close()

        # 6. Perform checkpoint to ensure we have blocks available in the middle of the file.
        self.session.checkpoint()

        # 7 & 8. Call compact. We expect that the overflow values at the end of the file are not
        #        rewritten and therefore the file size will mostly remain the same. Give a leeway
        #        of 10%.
        self.session.compact(self.uri)
        sizeAfterCompact = self.getSize()
        self.pr('After deleting values and compactions ' + str(sizeAfterCompact // mb) + 'MB')
        self.assertGreater(sizeAfterCompact, (sizeWithOverflow // 10) * 9)

        # Verify that we did indeed rewrote some pages but that didn't help with the file size.
        statDict = self.getCompactProgressStats()
        self.assertGreater(statDict["pages_reviewed"],0)
        self.assertGreater(statDict["pages_selected"],0)
        self.assertGreater(statDict["pages_rewritten"],0)

        # 9. Insert some normal values and expect that file size won't increase as free extents
        #    in the middle of the file will be used to write new data.

        # Insert around ~50% of the normal values in the table that we deleted earlier.
        c = self.session.open_cursor(self.uri, None)
        for i in range((self.nrecords // 100) * 25, (self.nrecords // 100) * 75):
            c.set_key(i)
            c.set_value(self.normalValue)
            self.assertEqual(c.update(),0)
        c.close()

        # Perform compact.
        self.session.compact(self.uri)

        # Test that the file size doesn't increase.
        sizeAfterNewInserts = self.getSize()
        self.pr('After Inserting bunch of values ' + str(sizeAfterNewInserts // mb) + 'MB')
        self.assertEqual(sizeAfterCompact, sizeAfterNewInserts)

if __name__ == '__main__':
    wttest.run()
