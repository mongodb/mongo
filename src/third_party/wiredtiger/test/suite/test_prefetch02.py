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
from collections import namedtuple
import helper, wiredtiger, wttest
from helper import WiredTigerCursor, statistic_uri
from suite_subprocess import suite_subprocess
from wtscenario import make_scenarios

# test_prefetch02.py
#    Run multiple scenarios which are expected to benefit from pre-fetching and ensure that
#    pre-fetching is running properly by checking various statistics. Additionally, run
#    multiple scenarios which should not trigger pre-fetching and check that these pages are
#    skipped when deciding whether to pre-fetch the pages.

PrefetchStats = namedtuple('PrefetchStats',
    ['pages_queued', 'prefetch_attempts', 'prefetch_attempts_succeeded', 'prefetch_pages_read'])

class test_prefetch02(wttest.WiredTigerTestCase, suite_subprocess):
    new_dir = 'new.dir'
    nrows = 100000
    uri = 'file:test_prefetch02'

    format_values = [
        ('col_var', dict(key_format='r')),
        ('row_int', dict(key_format='i')),
    ]

    value_format = 'i'

    conn_base_cfg = 'statistics=(all),cache_size=2GB,'

    config_options = [
        ('conn_default_on', dict(prefetch_cfg='prefetch=(available=true,default=true)',
                            session_cfg='', prefetch=True)),
        ('session_cfg', dict(prefetch_cfg='prefetch=(available=true,default=false)',
                            session_cfg='prefetch=(enabled=true)', prefetch=True)),
        ('prefetch_unavailable', dict(prefetch_cfg='prefetch=(available=false,default=false)',
                            session_cfg='', prefetch=False)),
        ('conn_default_on_null_session_cfg', dict(prefetch_cfg='prefetch=(available=true,default=true)',
                            session_cfg=None, prefetch=True)),
    ]

    prefetch_scenarios = [
        ('forward-traversal', dict(prefetch_scenario='forward-traversal', scenario_type='traversal')),
        ('backward-traversal', dict(prefetch_scenario='backward-traversal', scenario_type='traversal')),
        ('verify', dict(prefetch_scenario='verify', scenario_type='verify')),
    ]

    scenarios = make_scenarios(format_values, config_options, prefetch_scenarios)

    def conn_cfg(self):
        return self.conn_base_cfg + self.prefetch_cfg

    def get_prefetch_activity_stats(self, session):
        with WiredTigerCursor(session, statistic_uri()) as cursor:
            return PrefetchStats(
                pages_queued=cursor[wiredtiger.stat.conn.prefetch_pages_queued][2],
                prefetch_attempts=cursor[wiredtiger.stat.conn.prefetch_attempts][2],
                prefetch_attempts_succeeded=cursor[wiredtiger.stat.conn.prefetch_attempts_succeeded][2],
                prefetch_pages_read=cursor[wiredtiger.stat.conn.prefetch_pages_read][2],
            )

    # Checks for pre-fetching activity by asserting that relevant statistics have increased.
    def assert_prefetch_activity_increased(self, session, snapshot):
        current = self.get_prefetch_activity_stats(session)
        # FIXME-WT-12193 Change some of these statistic checks to use assertGreater instead if possible.
        self.assertGreaterEqual(current.pages_queued, snapshot.pages_queued)
        self.assertGreaterEqual(current.prefetch_attempts, snapshot.prefetch_attempts)
        self.assertGreaterEqual(current.prefetch_attempts_succeeded, snapshot.prefetch_attempts_succeeded)
        self.assertGreaterEqual(current.prefetch_pages_read, snapshot.prefetch_pages_read)

    # Checks that the values of statistics related to pre-fetching activity are equal to zero.
    def assert_no_prefetch_activity(self, session):
        stats = self.get_prefetch_activity_stats(session)
        self.assertEqual(stats.pages_queued, 0)
        self.assertEqual(stats.prefetch_attempts, 0)
        self.assertEqual(stats.prefetch_attempts_succeeded, 0)
        self.assertEqual(stats.prefetch_pages_read, 0)

    def _populate_table(self, session):
        session.create(self.uri, 'allocation_size=512,leaf_page_max=512,'
                        'key_format={},value_format={}'.format(self.key_format, self.value_format))
        cursor = session.open_cursor(self.uri)
        session.begin_transaction()
        for i in range(1, self.nrows):
            cursor[i] = i
        cursor.close()
        session.commit_transaction()
        session.checkpoint()

    def _run_traversal(self, session):
        cursor = session.open_cursor(self.uri)
        step = cursor.next if self.prefetch_scenario == 'forward-traversal' else cursor.prev

        # Traverse half the key space, snapshot stats, then finish the traversal.
        # If pre-fetching is unavailable, all stat counters should remain at zero.
        for _ in range(self.nrows // 2):
            step()
        snapshot = self.get_prefetch_activity_stats(session)

        ret = 0
        while ret == 0:
            ret = step()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.close()

        if self.prefetch:
            self.assert_prefetch_activity_increased(session, snapshot)
        else:
            self.assert_no_prefetch_activity(session)

    def _run_verify(self, conn):
        session_cfg = self.session_cfg if self.session_cfg is not None else ''
        session = conn.open_session(session_cfg)
        self.verifyUntilSuccess(session, self.uri)
        if self.prefetch:
            self.assert_prefetch_activity_increased(session, PrefetchStats(0, 0, 0, 0))
        else:
            self.assert_no_prefetch_activity(session)

    def test_prefetch_scenarios(self):
        os.mkdir(self.new_dir)
        helper.copy_wiredtiger_home(self, '.', self.new_dir)

        setup_conn = self.wiredtiger_open(self.new_dir, self.conn_cfg())
        self._populate_table(setup_conn.open_session(self.session_cfg))

        # Close and reopen the connection to evict all cached pages, so the subsequent
        # traversal reads from disk and triggers pre-fetching rather than serving from cache.
        setup_conn.close()
        new_conn = self.wiredtiger_open(self.new_dir, self.conn_cfg())

        if self.scenario_type == 'traversal':
            self._run_traversal(new_conn.open_session(self.session_cfg))
        elif self.scenario_type == 'verify':
            self._run_verify(new_conn)
