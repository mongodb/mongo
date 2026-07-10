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

import re, wiredtiger, wttest
from helper_disagg import DisaggConfigMixin, gen_disagg_storages
from wtscenario import make_scenarios

# test_repair01.py
#    Exercise the wiredtiger_repair() API (config errors, fetch_database_size, fetch_metadata) and
#    the related operations, in both non-disaggregated and disaggregated scenarios.
class test_repair01(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = 'statistics=(all),'
    scenarios = make_scenarios(gen_disagg_storages(disagg_only=False))

    def conn_config(self):
        if not self.is_disagg_scenario():
            return self.conn_base_config
        return self.conn_base_config + \
            'disaggregated=(page_log=%s,role="leader",lose_all_my_data=true),' % self.ds_name

    def conn_extensions(self, extlist):
        DisaggConfigMixin.conn_extensions(self, extlist)

    def repair(self, config):
        return wiredtiger.wiredtiger_repair(self.conn, config)

    @property
    def uri(self):
        return 'layered:tbl' if self.is_disagg_scenario() else 'table:tbl'

    def populate(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        for i in range(1000):
            cursor['key%06d' % i] = 'v' * 100
        cursor.close()
        self.session.checkpoint()

    def reported_size(self):
        result = self.repair('fetch_database_size=(local=true)')
        return int(re.search(r': (\d+)$', result).group(1))

    def checkpoint_size_fix(self, expect_triggered=False):
        pattern = r'disagg database size fix: recomputed database size -> \d+'
        assertion = self.assertRegex if expect_triggered else self.assertNotRegex

        self.conn.reconfigure('verbose=[disaggregated_storage:1]')
        try:
            with self.customStdoutPattern(lambda output: assertion(output, pattern)):
                self.session.checkpoint('debug=(database_size_fix=true)')
        finally:
            self.conn.reconfigure('verbose=[disaggregated_storage:0]')

    def test_config_errors(self):
        self.assertIn('wiredtiger_repair: empty config', self.repair(''))
        self.assertIn('No command found', self.repair('uri="table:tbl"'))

        if not self.is_disagg_scenario():
            return

        # fetch_database_size is checked first regardless of scenario, and always requires a
        # disagg connection with a picked-up checkpoint, so populate() first to get past that
        # guard and reach the collision check.
        self.populate()
        self.assertIn('Only one command is allowed', self.repair(
            'fetch_database_size=(local=true),fetch_metadata=(local=true)'))

    def test_fetch_metadata(self):
        self.populate()

        cursor = self.session.open_cursor('metadata:')
        cursor.set_key(self.uri)
        self.assertEqual(cursor.search(), 0)
        self.assertIn(f'{self.uri}: {cursor.get_value()}',
            self.repair(f'fetch_metadata=(local=true,uri="{self.uri}")'))
        cursor.close()

        self.assertIn(f'{self.uri}: key_format=S',
            self.repair(f'fetch_metadata=(local=true,uri="{self.uri}",key="key_format")'))
        self.assertIn(f'{self.uri}: <no "nope">',
            self.repair(f'fetch_metadata=(local=true,uri="{self.uri}",key="nope")'))
        self.assertIn('<no matching metadata entry for uri:"table:missing">',
            self.repair('fetch_metadata=(local=true,uri="table:missing")'))

        # Absent and empty uri/key must be equivalent, not "matches nothing".
        all_uris = self.repair('fetch_metadata=(local=true)')
        self.assertIn(f'{self.uri}: ', all_uris)
        self.assertNotIn('<no matching metadata entry', all_uris)
        self.assertEqual(all_uris, self.repair('fetch_metadata=(local=true,uri="")'))

        whole_value = self.repair(f'fetch_metadata=(local=true,uri="{self.uri}")')
        self.assertEqual(whole_value,
            self.repair(f'fetch_metadata=(local=true,uri="{self.uri}",key="")'))

        if self.is_disagg_scenario():
            self.assertIn(self.uri,
                self.repair(f'fetch_metadata=(local=false,uri="{self.uri}")'))
        else:
            self.assertIn('requires a disaggregated connection',
                self.repair('fetch_metadata=(local=false)'))

    def test_fetch_database_size(self):
        self.populate()

        if not self.is_disagg_scenario():
            self.assertIn('requires a disaggregated connection',
                self.repair('fetch_database_size=(local=true)'))
            self.assertIn('requires a disaggregated connection',
                self.repair('fetch_database_size=(local=false)'))
            return

        reported = self.reported_size()
        stat_size = self.get_stat(wiredtiger.stat.conn.disagg_database_size)
        self.assertEqual(reported, stat_size)
        self.assertGreater(reported, 0)

        # local=false recomputes from the metadata; absent drift it matches local=true exactly.
        self.assertIn(f'fetch_database_size(recompute): {stat_size}',
            self.repair('fetch_database_size=(local=false)'))

    def test_fix_size(self):
        self.populate()

        if not self.is_disagg_scenario():
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.checkpoint_size_fix(),
                '/requires a disaggregated leader connection/')
            return

        stat_size = self.get_stat(wiredtiger.stat.conn.disagg_database_size)

        # Absent any drift, the recompute matches the incrementally-tracked total.
        self.checkpoint_size_fix(expect_triggered=True)
        self.assertEqual(self.get_stat(wiredtiger.stat.conn.disagg_database_size), stat_size)

        # Drop a second, already-checkpointed table and grow the main one before fixing, so the
        # recompute has to reflect real change, not just replay the old total.
        extra_uri = 'layered:tbl_fix_size_extra'
        self.session.create(extra_uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(extra_uri)
        for i in range(50):
            cursor['key%06d' % i] = 'v' * 500
        cursor.close()
        self.session.checkpoint()  # settle first, or dropping it can hit its own dirty data

        pre_change_size = self.get_stat(wiredtiger.stat.conn.disagg_database_size)
        cursor = self.session.open_cursor(self.uri)
        for i in range(1000, 4000):
            cursor['key%06d' % i] = 'v' * 200
        cursor.close()
        self.session.drop(extra_uri)

        self.checkpoint_size_fix(expect_triggered=True)

        changed = self.reported_size()
        self.assertGreater(changed, pre_change_size)
        self.assertEqual(changed, self.get_stat(wiredtiger.stat.conn.disagg_database_size))

        # Cross-check against the independent __wt_verify_disagg_database_size path, only
        # reachable via verify_metadata=true at open.
        self.reopen_conn(config=self.conn_config() + 'verify_metadata=true,')
        self.ignoreStdoutPatternIfExists('Removing local file due to disagg mode')

        # A follower's session.checkpoint() is already a no-op skip at the session API layer
        # (standby has nothing to checkpoint), so it never reaches the leader-only guard in
        # __checkpoint_parse_config; it just needs to not raise or change the size.
        self.conn.reconfigure('disaggregated=(role="follower")')
        self.checkpoint_size_fix()
        self.assertEqual(self.get_stat(wiredtiger.stat.conn.disagg_database_size), changed)
