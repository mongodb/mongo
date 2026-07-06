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
#    Exercise the wiredtiger_repair() API for config-error paths, fetch_database_size, and
#    fetch_metadata. All run in non-disaggregated and disaggregated scenarios; the disagg scenario
#    additionally cross-validates the reported size against the disagg_database_size connection
#    statistic and exercises the shared (page-server-durable) metadata read.
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

    def test_config_errors(self):
        self.assertIn('wiredtiger_repair: empty config', self.repair(''))
        self.assertIn('No command found', self.repair('uri="table:tbl"'))
        # local=false so the collision is what fires, not the (now local=true-only) disagg guard.
        self.assertIn('Only one command is allowed', self.repair(
            'fetch_database_size=(local=false),fetch_metadata=(local=true)'))

    def test_fetch_metadata(self):
        self.populate()

        # A whole-value local fetch equals the metadata cursor's value for the same uri.
        cursor = self.session.open_cursor('metadata:')
        cursor.set_key(self.uri)
        self.assertEqual(cursor.search(), 0)
        self.assertIn(f'{self.uri}: {cursor.get_value()}',
            self.repair(f'fetch_metadata=(local=true,uri="{self.uri}")'))
        cursor.close()

        # A key-scoped fetch returns just that value; absent keys and uris are reported, not
        # errors.
        self.assertIn(f'{self.uri}: key_format=S',
            self.repair(f'fetch_metadata=(local=true,uri="{self.uri}",key="key_format")'))
        self.assertIn(f'{self.uri}: <no "nope">',
            self.repair(f'fetch_metadata=(local=true,uri="{self.uri}",key="nope")'))
        self.assertIn('<no matching metadata entry for uri:"table:missing">',
            self.repair('fetch_metadata=(local=true,uri="table:missing")'))

        # An empty uri/key is treated as absent, not as a literal target that matches nothing:
        # empty (or absent) uri means all URIs, empty (or absent) key means the whole value. The
        # empty and absent spellings must produce byte-identical reports.
        all_uris = self.repair('fetch_metadata=(local=true)')
        self.assertIn(f'{self.uri}: ', all_uris)
        self.assertNotIn('<no matching metadata entry', all_uris)
        self.assertEqual(all_uris, self.repair('fetch_metadata=(local=true,uri="")'))

        whole_value = self.repair(f'fetch_metadata=(local=true,uri="{self.uri}")')
        self.assertEqual(whole_value,
            self.repair(f'fetch_metadata=(local=true,uri="{self.uri}",key="")'))

        # The shared (page-server-durable) metadata read is disaggregated-only.
        if self.is_disagg_scenario():
            self.assertIn(self.uri,
                self.repair(f'fetch_metadata=(local=false,uri="{self.uri}")'))
        else:
            self.assertIn('requires a disaggregated connection',
                self.repair('fetch_metadata=(local=false)'))

    def test_fetch_database_size(self):
        self.populate()

        # local=false is not yet implemented (FIXME-WT-17945); unlike local=true it does not
        # require a disaggregated connection just to attempt the command.
        self.assertIn('not yet supported', self.repair('fetch_database_size=(local=false)'))

        if not self.is_disagg_scenario():
            self.assertIn('requires a disaggregated connection',
                self.repair('fetch_database_size=(local=true)'))
            return

        # Cross-validate against the disagg_database_size connection statistic.
        reported = self.reported_size()
        stat_size = self.get_stat(wiredtiger.stat.conn.disagg_database_size)
        self.assertEqual(reported, stat_size)
        self.assertGreater(reported, 0)
