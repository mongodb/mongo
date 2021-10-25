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
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet, ComplexDataSet
from wiredtiger import stat
from wtscenario import make_scenarios

# test_compact.py
#    session level compact operation
class test_compact(wttest.WiredTigerTestCase, suite_subprocess):
    name = 'test_compact'

    # Use a small page size because we want to create lots of pages.
    config = 'allocation_size=512,' +\
        'leaf_page_max=512,key_format=S'
    nentries = 50000

    # The table is a complex object, give it roughly 5 pages per underlying
    # file.
    types = [
        ('file', dict(type='file:', dataset=SimpleDataSet, maxpages=5)),
        ('table', dict(type='table:', dataset=ComplexDataSet, maxpages=50))
        ]
    compact = [
        ('method', dict(utility=0,reopen=0)),
        ('method_reopen', dict(utility=0,reopen=1)),
        ('utility', dict(utility=1,reopen=0)),
    ]
    scenarios = make_scenarios(types, compact)

    # Configure the connection so that eviction doesn't happen (which could
    # skew our compaction results).
    conn_config = 'cache_size=1GB,eviction_checkpoint_target=80,' +\
        'eviction_dirty_target=80,eviction_dirty_trigger=95,statistics=(all)'

    # Return stats that track the progress of compaction.
    def getCompactProgressStats(self, uri):
        cstat = self.session.open_cursor(
            'statistics:' + uri, None, 'statistics=(all)')
        statDict = {}
        statDict["pages_reviewed"] = cstat[stat.dsrc.btree_compact_pages_reviewed][2]
        statDict["pages_skipped"] = cstat[stat.dsrc.btree_compact_pages_skipped][2]
        statDict["pages_selected"] = cstat[stat.dsrc.btree_compact_pages_write_selected][2]
        statDict["pages_rewritten"] = cstat[stat.dsrc.btree_compact_pages_rewritten][2]
        cstat.close()
        return statDict

    # Test compaction.
    def test_compact(self):
        # FIXME-WT-7187
        # This test is temporarily disabled for OS/X, it fails often, but not consistently.
        import platform
        if platform.system() == 'Darwin':
            self.skipTest('Compaction tests skipped, as they fail on OS/X')

        # Populate an object
        uri = self.type + self.name
        ds = self.dataset(self, uri, self.nentries - 1, config=self.config)
        ds.populate()

        # Reopen the connection to force the object to disk.
        self.reopen_conn()

        # Confirm the tree starts big
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, None)
        self.assertGreater(stat_cursor[stat.dsrc.btree_row_leaf][2], self.maxpages)
        stat_cursor.close()

        # Remove most of the object.
        c1 = self.session.open_cursor(uri, None)
        c1.set_key(ds.key(5))
        c2 = self.session.open_cursor(uri, None)
        c2.set_key(ds.key(self.nentries - 5))
        self.session.truncate(None, c1, c2, None)
        c1.close()
        c2.close()

        # Compact it, using either the session method or the utility.
        if self.utility == 1:
            self.session.checkpoint(None)
            self.close_conn()
            self.runWt(["compact", uri])
        else:
            # Optionally reopen the connection so we do more on-disk tests.
            if self.reopen == 1:
                self.session.checkpoint(None)
                self.reopen_conn()

            self.session.compact(uri, None)

        # Verify compact progress stats. We can't do this with utility method as reopening the
        # connection would reset the stats.
        if self.utility != 1:
            statDict = self.getCompactProgressStats(uri)
            self.assertGreater(statDict["pages_reviewed"],0)
            self.assertGreater(statDict["pages_selected"],0)
            self.assertGreater(statDict["pages_rewritten"],0)

        # Confirm compaction worked: check the number of on-disk pages
        self.reopen_conn()
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, None)
        self.assertLess(stat_cursor[stat.dsrc.btree_row_leaf][2], self.maxpages)
        stat_cursor.close()

if __name__ == '__main__':
    wttest.run()
