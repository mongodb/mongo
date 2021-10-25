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
# test_compact02.py
#   Test that compact reduces the file size.
#

import time, wiredtiger, wttest
from wiredtiger import stat
from wtscenario import make_scenarios

# Test basic compression
class test_compact02(wttest.WiredTigerTestCase):

    types = [
        ('table', dict(uri='table:test_compact02')),
    ]
    cacheSize = [
        ('default', dict(cacheSize='')),
        ('1mb', dict(cacheSize='cache_size=1MB')),
        ('10gb', dict(cacheSize='cache_size=10GB')),
    ]

    # There's a balance between the pages we create and the size of the records
    # being stored: compaction doesn't work on tables with many overflow items
    # because we don't rewrite them. Experimentally, 8KB is as small as the test
    # can go. Additionally, we can't set the maximum page size too large because
    # there won't be enough pages to rewrite. Experimentally, 128KB works.
    fileConfig = [
        ('default', dict(fileConfig='')),
        ('8KB', dict(fileConfig='leaf_page_max=8kb')),
        ('64KB', dict(fileConfig='leaf_page_max=64KB')),
        ('128KB', dict(fileConfig='leaf_page_max=128KB')),
    ]
    scenarios = make_scenarios(types, cacheSize, fileConfig)

    # We want about 22K records that total about 130Mb.  That is an average
    # of 6196 bytes per record.  Half the records should be smaller, about
    # 2700 bytes (about 30Mb) and the other half should be larger, 9666 bytes
    # per record (about 100Mb).
    #
    # Test flow is as follows.
    #
    # 1. Create a table with the data, alternating record size.
    # 2. Checkpoint and get stats on the table to confirm the size.
    # 3. Delete the half of the records with the larger record size.
    # 4. Checkpoint so compact finds something to work with.
    # 5. Call compact.
    # 6. Get stats on compacted table.
    #
    nrecords = 22000
    bigvalue = "abcdefghi" * 1074          # 9*1074 == 9666
    smallvalue = "ihgfedcba" * 303         # 9*303 == 2727

    fullsize = nrecords // 2 * len(bigvalue) + nrecords // 2 * len(smallvalue)

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

    # This test varies the cache size and so needs to set up its own connection.
    # Override the standard methods.
    def setUpConnectionOpen(self, dir):
        return None
    def setUpSessionOpen(self, conn):
        return None
    def ConnectionOpen(self, cacheSize):
        self.home = '.'
        conn_params = 'create,' + \
            cacheSize + ',error_prefix="%s",' % self.shortid() + \
            'statistics=(all),' + \
            'eviction_dirty_target=99,eviction_dirty_trigger=99'
        try:
            self.conn = wiredtiger.wiredtiger_open(self.home, conn_params)
        except wiredtiger.WiredTigerError as e:
            print("Failed conn at '%s' with config '%s'" % (dir, conn_params))
        self.session = self.conn.open_session(None)

    # Create a table, add keys with both big and small values.
    def test_compact02(self):
        mb = 1024 * 1024

        # FIXME-WT-7187
        # This test is temporarily disabled for OS/X, it fails, but not consistently.
        import platform
        if platform.system() == 'Darwin':
            self.skipTest('Compaction tests skipped, as they fail on OS/X')

        self.ConnectionOpen(self.cacheSize)

        # Set the leaf_value_max to ensure we never create overflow items.
        # FIXME: WT-2298
        params = 'key_format=i,value_format=S,leaf_value_max=10MB,' + self.fileConfig

        # 1. Create a table with the data, alternating record size.
        self.session.create(self.uri, params)
        c = self.session.open_cursor(self.uri, None)
        for i in range(self.nrecords):
            if i % 2 == 0:
                c[i] = str(i) + self.bigvalue
            else:
                c[i] = str(i) + self.smallvalue
        c.close()

        # 2. Checkpoint and get stats on the table to confirm the size.
        self.session.checkpoint()
        sz = self.getSize()
        self.pr('After populate ' + str(sz // mb) + 'MB')
        self.assertGreater(sz, self.fullsize)

        # 3. Delete the half of the records with the larger record size.
        c = self.session.open_cursor(self.uri, None)
        count = 0
        for i in range(self.nrecords):
            if i % 2 == 0:
                count += 1
                c.set_key(i)
                c.remove()
        c.close()
        self.pr('Removed total ' + str((count * 9666) // mb) + 'MB')

        # 4. Checkpoint
        self.session.checkpoint()

        # 5. Call compact.
        # Compact can collide with eviction, if that happens we retry. Wait for
        # a long time, the check for EBUSY means we're not retrying on any real
        # errors.
        for i in range(1, 100):
            if not self.raisesBusy(
              lambda: self.session.compact(self.uri, None)):
                break
            time.sleep(6)

        # 6. Get stats on compacted table.
        sz = self.getSize()
        self.pr('After compact ' + str(sz // mb) + 'MB')

        # After compact, the file size should be less than half the full size.
        self.assertLess(sz, self.fullsize // 2)

        # Verify compact progress stats.
        statDict = self.getCompactProgressStats()
        self.assertGreater(statDict["pages_reviewed"],0)
        self.assertGreater(statDict["pages_selected"],0)
        self.assertGreater(statDict["pages_rewritten"],0)

if __name__ == '__main__':
    wttest.run()
