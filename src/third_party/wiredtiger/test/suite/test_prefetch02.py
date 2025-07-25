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

import os
import helper, wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

# test_prefetch02.py
#    Run multiple scenarios which are expected to benefit from pre-fetching and ensure that
#    pre-fetching is running properly by checking various statistics. Additionally, run
#    multiple scenarios which should not trigger pre-fetching and check that these pages are
#    skipped when deciding whether to pre-fetch the pages.

class test_prefetch02(wttest.WiredTigerTestCase, suite_subprocess):
    new_dir = 'new.dir'
    nrows = 100000
    uri = 'file:test_prefetch02'

    format_values = [
        ('col_var', dict(key_format='r', value_format='i')),
        ('col_fix', dict(key_format='r', value_format='8t')),
        ('row_int', dict(key_format='i', value_format='i')),
    ]

    config_options = [
        ('config_a', dict(conn_cfg='prefetch=(available=true,default=true),statistics=(all)',
                            session_cfg='', prefetch=True)),
        ('config_b', dict(conn_cfg='prefetch=(available=true,default=false),statistics=(all)',
                            session_cfg='prefetch=(enabled=true)', prefetch=True)),
        ('config_c', dict(conn_cfg='prefetch=(available=false,default=false),statistics=(all)',
                            session_cfg='', prefetch=False)),
    ]

    prefetch_scenarios = [
        ('forward-traversal', dict(prefetch_scenario='forward-traversal', scenario_type='traversal')),
        ('backward-traversal', dict(prefetch_scenario='backward-traversal', scenario_type='traversal')),
        ('verify', dict(prefetch_scenario='verify', scenario_type='verify')),
    ]

    scenarios = make_scenarios(format_values, config_options, prefetch_scenarios)

    def get_stat(self, stat, session_name):
        stat_cursor = session_name.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def get_prefetch_activity_stats(self, session_name):
        pages_queued = self.get_stat(wiredtiger.stat.conn.prefetch_pages_queued, session_name)
        prefetch_attempts = self.get_stat(wiredtiger.stat.conn.prefetch_attempts, session_name)
        prefetch_pages_read = self.get_stat(wiredtiger.stat.conn.prefetch_pages_read, session_name)
        return pages_queued, prefetch_attempts, prefetch_pages_read

    def get_prefetch_skipped_stat(self, session_name):
        prefetch_skips = self.get_stat(wiredtiger.stat.conn.prefetch_skipped, session_name)
        return prefetch_skips

    # Checks for pre-fetching activity by asserting that relevant statistics have increased.
    def check_prefetching_activity(self, session_name, pages_queued, prefetch_attempts, prefetch_pages_read):
        new_pages_queued, new_prefetch_attempts, new_prefetch_pages_read = self.get_prefetch_activity_stats(session_name)

        # FIXME-WT-12193 Change some of these statistic checks to use assertGreaterEqual instead if possible.
        self.assertGreaterEqual(new_pages_queued, pages_queued)
        self.assertGreaterEqual(new_prefetch_attempts, prefetch_attempts)
        self.assertGreaterEqual(new_prefetch_pages_read, prefetch_pages_read)

    # Checks that the values of statistics related to pre-fetching activity are equal to zero,
    # and that pages are being skipped when deciding whether to pre-fetch the page or not.
    def check_no_prefetching_activity(self, session_name, prefetch_skips):
        new_prefetch_skips = self.get_prefetch_skipped_stat(session_name)
        pages_queued, prefetch_attempts, prefetch_pages_read = self.get_prefetch_activity_stats(session_name)
        self.assertGreaterEqual(new_prefetch_skips, prefetch_skips)
        self.assertEqual(pages_queued, 0)
        self.assertEqual(prefetch_attempts, 0)
        self.assertEqual(prefetch_pages_read, 0)

    def test_prefetch_scenarios(self):
        os.mkdir(self.new_dir)
        helper.copy_wiredtiger_home(self, '.', self.new_dir)

        new_conn = self.wiredtiger_open(self.new_dir, self.conn_cfg)
        s = new_conn.open_session(self.session_cfg)
        s.create(self.uri, 'allocation_size=512,leaf_page_max=512,'
                        'key_format={},value_format={}'.format(self.key_format, self.value_format))
        c1 = s.open_cursor(self.uri)
        s.begin_transaction()
        for i in range(1, self.nrows):
            if self.value_format == '8t':
                c1[i] = 100
            else:
                c1[i] = i
        c1.close()
        s.commit_transaction()
        s.checkpoint()

        if self.scenario_type == 'traversal':
            c2 = s.open_cursor(self.uri)

            # Traverse through half the key space and collect pre-fetching statistics. Then, traverse
            # through the rest of the keys and check that the relevant pre-fetching statistics have
            # increased by the end. If pre-fetching is not available, check that we are skipping pages.
            for i in range(self.nrows // 2):
                ret = c2.next() if self.prefetch_scenario == 'forward-traversal' else c2.prev()
            pages_queued, prefetch_attempts, prefetch_pages_read = self.get_prefetch_activity_stats(s)
            prefetch_skips = self.get_prefetch_skipped_stat(s)

            while True:
                ret = c2.next() if self.prefetch_scenario == 'forward-traversal' else c2.prev()
                if ret != 0:
                    break
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
            c2.close()

            if self.prefetch:
                self.check_prefetching_activity(s, pages_queued, prefetch_attempts, prefetch_pages_read)
            else:
                self.check_no_prefetching_activity(s, prefetch_skips)

        elif self.scenario_type == 'verify':
            if self.prefetch:
                verify_session = new_conn.open_session("prefetch=(enabled=true)")
                self.verifyUntilSuccess(verify_session, self.uri)
                self.check_prefetching_activity(verify_session, 0, 0, 0)
            else:
                verify_session = new_conn.open_session("")
                self.verifyUntilSuccess(verify_session, self.uri)
                self.check_no_prefetching_activity(verify_session, 0)
