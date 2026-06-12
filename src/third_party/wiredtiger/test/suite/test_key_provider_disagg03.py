#!/usr/bin/env python3
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
import re, os, subprocess, json
import wttest
from run import wt_builddir
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages, get_shard_id
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_key_provider_disagg03.py
#    Push-mode key provider smoke test: verify the pushed key is durably persisted
#    to the turtle key-provider page after a checkpoint.
@disagg_test_class
class test_key_provider_disagg03(wttest.WiredTigerTestCase):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages('test_key_provider_disagg03', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    MAIN_KEK_PAGE_ID = 1
    EXPECTED_KEK_VERSION = 1

    WT_SPECIAL_PALI_TURTLE_FILE_ID = 2
    WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID = 26

    turtle_table = f'pages_{get_shard_id(WT_SPECIAL_PALI_TURTLE_FILE_ID):02d}.db'
    key_provider_table = f'pages_{get_shard_id(WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID):02d}.db'

    uri = "layered:test_key_provider_disagg03"

    def conn_extensions(self, extlist):
        config = '=(early_load=true,config=\"verbose=-1,version=1\")'
        extlist.extension('test', "key_provider" + config)
        DisaggConfigMixin.conn_extensions(self, extlist)

    def sqlite_fetch_information(self, home, database, sql_query):
        sqlite_exe = os.path.join(wt_builddir, 'sqlite3')
        database_home = os.path.join(home, 'kv_home', database)
        result = subprocess.run(
            [sqlite_exe, '-json', database_home, sql_query],
            capture_output=True, text=True, check=True)
        return json.loads(result.stdout)[0]

    def key_provider_page_count(self, home="."):
        return self.sqlite_fetch_information(
            home, self.key_provider_table,
            f'SELECT COUNT(*) FROM pages WHERE table_id={self.WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID};'
        )['COUNT(*)']

    def validate_meta_file(self, home="."):
        result = self.sqlite_fetch_information(
            home, self.turtle_table,
            f'''SELECT * FROM pages
                WHERE table_id={self.WT_SPECIAL_PALI_TURTLE_FILE_ID}
                ORDER BY lsn DESC LIMIT 1;'''
        )
        m = re.search(".*page_id=(\d+),lsn=(\d+).*version=(\d+)", result['page_data'])
        self.assertTrue(m)
        page_id, version = int(m.group(1)), int(m.group(3))
        self.assertEqual(page_id, self.MAIN_KEK_PAGE_ID)
        self.assertEqual(version, self.EXPECTED_KEK_VERSION)

    def test_set_key_persists(self):
        if (self.ds_name != "palite"):
            self.skipTest("Must use PALite to verify contents")

        ds = SimpleDataSet(self, self.uri, 10)
        ds.populate()
        # A pushed key is only persisted once the stable timestamp reaches it. Advance stable so the
        # checkpoint selects the earliest pushed key.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

        self.assertGreaterEqual(self.key_provider_page_count(), 1)
        self.validate_meta_file()
