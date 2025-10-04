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
# test_compact04.py
#   Test the accuracy of compact work estimation.
#

from wiredtiger import stat
from compact_util import compact_util

# Test the accuracy of compact work estimation.
class test_compact04(compact_util):

    # Keep debug messages on by default. This is useful for diagnosing spurious test failures.
    conn_config = 'statistics=(all),cache_size=100MB,verbose=(compact_progress,compact:4)'
    create_params = 'key_format=i,value_format=S,allocation_size=4KB,leaf_page_max=32KB,'
    table_numkv = 100 * 1000
    table_uri_prefix='table:test_compact04-'

    delete_range_len = 10 * 1000
    delete_ranges_count = 4

    # Create the table in a way that it creates a mostly empty file.
    def test_compact04(self):

        # Run the test several times. It is practically impossible to design a progress prediction
        # mechanism that is always correct, so it is possible to get a spurious prediction that is
        # considerably less accurate than we'd like. We just need to make sure that we don't get
        # those wrong predictions too frequently.
        num_failures = 0
        num_successes = 0

        for iteration in range(0, 10):
            table_uri = self.table_uri_prefix + str(iteration)

            # Create the table and populate it with a lot of data
            self.session.create(table_uri, self.create_params)
            self.populate(table_uri, 0, self.table_numkv)
            self.session.checkpoint()

            # Now let's delete a lot of data ranges. Create enough space so that compact runs in more
            # than one iteration.
            c = self.session.open_cursor(table_uri, None)
            for r in range(self.delete_ranges_count):
                start = r * self.table_numkv // self.delete_ranges_count
                for i in range(self.delete_range_len):
                    c.set_key(start + i)
                    c.remove()
            c.close()

            # Compact!
            self.session.compact(table_uri, None)
            self.ignoreStdoutPatternIfExists('WT_VERB_COMPACT')
            self.ignoreStderrPatternIfExists('WT_VERB_COMPACT')

            # Verify the compact progress stats.
            c_stat = self.session.open_cursor('statistics:' + table_uri, None, 'statistics=(all)')
            pages_rewritten = c_stat[stat.dsrc.btree_compact_pages_rewritten][2]
            pages_rewritten_expected = c_stat[stat.dsrc.btree_compact_pages_rewritten_expected][2]
            c_stat.close()

            # Compact stats can be retrieved with tiered storage but they're not meaningful.
            # So if we're running tiered gather the stats but return before all the computation.
            if self.runningHook('tiered'):
                return

            self.assertGreater(pages_rewritten, 0)
            self.assertGreater(pages_rewritten_expected, 0)

            d = abs(pages_rewritten - pages_rewritten_expected) / pages_rewritten

            # Check whether we succeeded. Terminate the test early if things are going well. If we
            # experience even one failure, run through all iterations to ensure that the failures
            # are rare. Each iteration runs on the order of seconds, so this test will complete
            # quickly even if we have to run through all iterations.
            message = 'Compacting %s: Prediction error: %0.2f%%' % (table_uri, d * 100)
            if d < 0.15:
                self.pr(message)
                num_successes += 1
                if num_successes >= 1 and num_failures == 0:
                    self.pr('Finishing the test early')
                    return
            else:
                self.pr(message + ' (FAILURE)')
                num_failures += 1
                self.assertLessEqual(num_failures, 2)
