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

import json, os, struct, subprocess
import wttest
from run import wt_builddir
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages, get_shard_id
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_key_provider_disagg05.py
#    Push-mode key provider: exercise the pending-key list across multiple checkpoints and
#    verify each checkpoint persists a new key-provider page.
@disagg_test_class
class test_key_provider_disagg05(wttest.WiredTigerTestCase):
    conn_base_config = ',create,statistics=(all),statistics_log=(wait=1,json=true,on_close=true),'
    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages('test_key_provider_disagg05', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    MAIN_KEK_PAGE_ID = 1
    WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID = 26
    key_provider_table = f'pages_{get_shard_id(WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID):02d}.db'

    uri = "layered:test_key_provider_disagg05"

    def conn_extensions(self, extlist):
        config = '=(early_load=true,config=\"verbose=-1,version=1,key_expires=0\")'
        extlist.extension('test', "key_provider" + config)
        DisaggConfigMixin.conn_extensions(self, extlist)

    def sqlite_fetch(self, query):
        sqlite_exe = os.path.join(wt_builddir, 'sqlite3')
        database = os.path.join(self.home, 'kv_home', self.key_provider_table)
        result = subprocess.run([sqlite_exe, '-json', database, query],
            capture_output=True, text=True, check=True)
        return json.loads(result.stdout)

    # The pushed timestamp lives at offset 16 of the WT_CRYPT_HEADER (8 bytes, little-endian).
    CRYPT_HEADER_TIMESTAMP_OFFSET = 16

    def key_provider_pages(self):
        # Read each persisted page ordered by LSN, with a hex dump of its bytes.
        return self.sqlite_fetch(
            f'SELECT lsn, page_id, hex(page_data) AS hex FROM pages '
            f'WHERE table_id={self.WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID} '
            f'ORDER BY lsn ASC;')

    def header_timestamp(self, hex_page):
        # The WT_CRYPT_HEADER stores the pushed timestamp as a little-endian uint64 at byte offset 16.
        data = bytes.fromhex(hex_page)
        return struct.unpack_from('<Q', data, self.CRYPT_HEADER_TIMESTAMP_OFFSET)[0]

    def validate_key_provider_pages(self, expected_count_min):
        # Every page references the main KEK page, and both the LSN and the pushed timestamp must
        # strictly increase across pages.
        rows = self.key_provider_pages()
        self.assertGreaterEqual(len(rows), expected_count_min)
        previous_lsn = -1
        previous_ts = -1
        for row in rows:
            self.assertEqual(row['page_id'], self.MAIN_KEK_PAGE_ID)
            self.assertGreater(row['lsn'], previous_lsn)
            timestamp = self.header_timestamp(row['hex'])
            self.assertGreater(timestamp, previous_ts)
            previous_lsn = row['lsn']
            previous_ts = timestamp

    def test_multiple_pushes_across_checkpoints(self):
        # Each checkpoint pushes a fresh key onto the pending list and drains it, persisting one
        # new key-provider page.
        if self.ds_name != "palite":
            self.skipTest("Must use PALite to verify contents")

        ds = SimpleDataSet(self, self.uri, 10)
        ds.populate()

        # The first checkpoint creates the page table; record the baseline page count.
        stable = 1
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(stable))
        self.session.checkpoint()
        baseline = len(self.key_provider_pages())
        self.assertGreaterEqual(baseline, 1)

        # Write and checkpoint a few more times; each checkpoint persists another page.
        cursor = self.session.open_cursor(self.uri)
        for i in range(4):
            cursor[ds.key(100 + i)] = ds.value(100 + i)
            stable += 1
            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(stable))
            self.session.checkpoint()
        cursor.close()

        # The page count must have grown past the baseline.
        self.validate_key_provider_pages(baseline + 1)
