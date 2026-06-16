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

import json, os, subprocess
import wiredtiger, wttest
from run import wt_builddir
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages, get_shard_id
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_key_provider_disagg05.py
#    Push-mode key provider: exercise the pending-key list across multiple checkpoints and
#    verify each checkpoint persists a new key-provider page.
@disagg_test_class
class test_key_provider_disagg05(wttest.WiredTigerTestCase):
    conn_base_config = ',create,statistics=(all),'
    def conn_config(self):
        return self.extensionsConfig() + self.conn_base_config + 'disaggregated=(role="leader")'

    disagg_storages = gen_disagg_storages('test_key_provider_disagg05', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    MAIN_KEK_PAGE_ID = 1
    # Byte offset of the header_size field within WT_CRYPT_HEADER (see crypt_header.h).
    CRYPT_HEADER_SIZE_OFFSET = 6
    WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID = 26
    key_provider_table = f'pages_{get_shard_id(WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID):02d}.db'

    uri = "layered:test_key_provider_disagg05"

    # Base bytes for each pushed key; key_for() appends a unique per-push suffix.
    KEY_PREFIX = b'abcdefghijklmnopqrstuvwxyz'

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

    def key_provider_pages(self):
        # Read each persisted page ordered by LSN, exposing the persisted key as a named field so
        # callers don't parse the raw hex dump.
        rows = self.sqlite_fetch(
            f'SELECT lsn, page_id, hex(page_data) AS hex FROM pages '
            f'WHERE table_id={self.WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID} '
            f'ORDER BY lsn ASC;')
        for row in rows:
            # Each page is the crypt header followed by the key; expose the key under 'key'.
            data = bytes.fromhex(row['hex'])
            header_size = data[self.CRYPT_HEADER_SIZE_OFFSET]
            row['key'] = data[header_size:]
        return rows

    def validate_latest_key_provider_page(self, timestamp):
        # Validate the most recently persisted key-provider page: it is on the main KEK page and
        # holds the key pushed for this checkpoint (which encodes the timestamp).
        rows = self.key_provider_pages()
        self.assertGreater(len(rows), 0)
        latest = rows[-1]
        self.assertEqual(latest['page_id'], self.MAIN_KEK_PAGE_ID)
        self.assertEqual(latest['key'], self.key_for(timestamp))

    def key_for(self, timestamp):
        # A unique key per push: the key prefix followed by the push timestamp.
        return self.KEY_PREFIX + str(timestamp).encode()

    def push_key(self, timestamp):
        crypt = wiredtiger.CryptKeys()
        crypt.keys = self.key_for(timestamp)
        crypt.timestamp = timestamp
        self.assertEqual(self.conn.get_key_provider().set_key(self.session, crypt), 0)

    def test_multiple_pushes_across_checkpoints(self):
        if self.ds_name != "palite":
            self.skipTest("Must use PALite to verify contents")

        ds = SimpleDataSet(self, self.uri, 10)
        ds.populate()
        cursor = self.session.open_cursor(self.uri)

        # Each checkpoint writes a row (so it does real work) and persists the most recent pending
        # key whose timestamp is at or below the stable timestamp as a new key-provider page.

        # Queue three keys, then advance stable to 3 so the checkpoint selects the most recent
        # (timestamp 3) and drains the consumed entries.
        self.push_key(1)
        self.push_key(2)
        self.push_key(3)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(3))
        cursor[ds.key(101)] = ds.value(101)
        self.session.checkpoint()
        self.validate_latest_key_provider_page(timestamp=3)

        # Push a single key and advance stable to it: the checkpoint persists it (timestamp 4).
        self.push_key(4)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(4))
        cursor[ds.key(102)] = ds.value(102)
        self.session.checkpoint()
        self.validate_latest_key_provider_page(timestamp=4)

        # Push another key and advance stable to it: the checkpoint persists it (timestamp 5).
        self.push_key(5)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(5))
        cursor[ds.key(103)] = ds.value(103)
        self.session.checkpoint()
        self.validate_latest_key_provider_page(timestamp=5)

        cursor.close()

    def test_select_highest_at_or_below_stable(self):
        if self.ds_name != "palite":
            self.skipTest("Must use PALite to verify contents")

        ds = SimpleDataSet(self, self.uri, 10)
        ds.populate()
        cursor = self.session.open_cursor(self.uri)

        # Queue three keys but advance stable only to 2: the checkpoint must select the highest
        # key at or below stable (timestamp 2), leaving the timestamp-3 key pending.
        self.push_key(1)
        self.push_key(2)
        self.push_key(3)
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        cursor[ds.key(101)] = ds.value(101)
        self.session.checkpoint()
        self.validate_latest_key_provider_page(timestamp=2)

        # Advance stable to 3: the still-pending timestamp-3 key is now selected.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(3))
        cursor[ds.key(102)] = ds.value(102)
        self.session.checkpoint()
        self.validate_latest_key_provider_page(timestamp=3)

        cursor.close()
