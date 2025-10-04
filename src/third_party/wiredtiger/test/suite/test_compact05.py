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
# test_compact05.py
#   Test if foreground compaction proceeds depending on the free_space_target field.
#

import wttest
from wiredtiger import stat
from wtscenario import make_scenarios
from compact_util import compact_util

class test_compact05(compact_util):

    free_space_target = [
        # The threshold is smaller than the number of available bytes, compaction should proceed.
        ('1MB', dict(compact_config='free_space_target=1MB',
                     expected_compaction=True,
                     expected_stdout=None)),
        # The threshold is greater than the number of available bytes, compaction should not proceed.
        ('45MB', dict(compact_config='free_space_target=45MB',
                      expected_compaction=False,
                      expected_stdout='number of available bytes.*is less than the configured threshold')),
    ]
    scenarios = make_scenarios(free_space_target)

    conn_config = 'statistics=(all),verbose=(compact_progress,compact)'
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    table_numkv = 100 * 1000
    table_uri='table:test_compact05'

    delete_range_len = 10 * 1000
    delete_ranges_count = 4

    # Create the table in a way that it creates a mostly empty file.
    def test_compact05(self):

        if self.runningHook('tiered'):
            self.skipTest("Tiered tables do not support compaction")

        # Create the table and populate it with a lot of data.
        self.session.create(self.table_uri, self.create_params)
        self.populate(self.table_uri, 0, self.table_numkv)
        self.session.checkpoint()

        # Now let's delete a lot of data ranges. Create enough space so that compact runs in more
        # than one iteration.
        c = self.session.open_cursor(self.table_uri, None)
        for r in range(self.delete_ranges_count):
            start = r * self.table_numkv // self.delete_ranges_count
            for i in range(self.delete_range_len):
                c.set_key(start + i)
                c.remove()
        c.close()

        # Compact!
        if self.expected_compaction:
            # Compaction should succeed, ignore all the logs.
            self.session.compact(self.table_uri, self.compact_config)
            self.ignoreStdoutPatternIfExists('WT_VERB_COMPACT')
            self.ignoreStderrPatternIfExists('WT_VERB_COMPACT')
        else:
            # Compaction should fail with a specific error.
            with self.expectedStdoutPattern(self.expected_stdout):
                self.session.compact(self.table_uri, self.compact_config)

        # Verify the compact progress stats.
        c_stat = self.session.open_cursor('statistics:' + self.table_uri, None, 'statistics=(all)')
        pages_rewritten = c_stat[stat.dsrc.btree_compact_pages_rewritten][2]
        pages_rewritten_expected = c_stat[stat.dsrc.btree_compact_pages_rewritten_expected][2]
        c_stat.close()

        if self.expected_compaction:
            self.assertGreater(pages_rewritten, 0)
            self.assertGreater(pages_rewritten_expected, 0)
        else:
            self.assertEqual(pages_rewritten, 0)
            self.assertEqual(pages_rewritten_expected, 0)
