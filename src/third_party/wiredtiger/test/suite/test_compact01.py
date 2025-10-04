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
from compact_util import compact_util
from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet, ComplexDataSet
from wiredtiger import stat
from wtscenario import make_scenarios

# test_compact.py
#    session level compact operation
class test_compact(compact_util, suite_subprocess):
    name = 'test_compact'

    # We don't want to set the page size too small as compaction doesn't work on tables with many
    # overflow items, furthermore eviction can get very slow with overflow items. We don't want the
    # page size to be too big either as there won't be enough pages to rewrite.
    config = 'leaf_page_max=8KB,key_format=S'
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

    # Test compaction.
    @wttest.skip_for_hook("timestamp", "removing timestamped items will not free space")
    def test_compact(self):
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
        c1 = ds.open_cursor(uri, None)
        c1.set_key(ds.key(5))
        c2 = ds.open_cursor(uri, None)
        c2.set_key(ds.key(self.nentries - 5))
        ds.truncate(None, c1, c2, None)
        c1.close()
        c2.close()

        # Compact it, using either the session method or the utility. Generated files are ~2MB, set
        # the minimum threshold to a low value to make sure compaction can be executed.
        compact_cfg = "free_space_target=1MB"
        if self.utility == 1:
            self.session.checkpoint(None)
            self.close_conn()
            self.runWt(["compact", "-c", compact_cfg, uri])
        else:
            # Optionally reopen the connection so we do more on-disk tests.
            if self.reopen == 1:
                self.session.checkpoint(None)
                self.reopen_conn()

            self.session.compact(uri, compact_cfg)

        # Verify compact progress stats. We can't do this with utility method as reopening the
        # connection would reset the stats.
        if self.utility == 0 and self.reopen == 0 and not self.runningHook('tiered'):
            statDict = self.get_compact_progress_stats(uri)
            self.assertGreater(statDict["pages_reviewed"],0)
            self.assertGreater(statDict["pages_rewritten"],0)
            self.assertEqual(statDict["pages_rewritten"] + statDict["pages_skipped"],
                                statDict["pages_reviewed"])

        # Confirm compaction worked: check the number of on-disk pages
        self.reopen_conn()
        stat_cursor = self.session.open_cursor('statistics:' + uri, None, None)
        self.assertLess(stat_cursor[stat.dsrc.btree_row_leaf][2], self.maxpages)
        stat_cursor.close()
