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

import json, os, re, subprocess
import wiredtiger, wttest
from run import wt_builddir
from helper_disagg import DisaggConfigMixin, get_shard_id

# Shared base for the key-provider tests.
class KeyProviderBase(wttest.WiredTigerTestCase):
    # Per-test knobs.
    key_provider_version = 1   # 0 = pull (get_key), 1 = push (set_key)
    key_expires = 0

    # Constants.
    KEY_PREFIX = b'abcdefghijklmnopqrstuvwxyz'
    WT_SPECIAL_PALI_TURTLE_FILE_ID = 2
    WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID = 26
    MAIN_KEK_PAGE_ID = 1
    CRYPT_HEADER_SIZE_OFFSET = 6
    CRYPT_HEADER_TIMESTAMP_OFFSET = 16
    TURTLE_KEK_VERSION = 1
    turtle_table = f'pages_{get_shard_id(WT_SPECIAL_PALI_TURTLE_FILE_ID):02d}.db'
    key_provider_table = f'pages_{get_shard_id(WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID):02d}.db'

    def setUp(self):
        # The tests inspect the persisted pages, which only PALite exposes.
        if self.ds_name != "palite":
            self.skipTest("Must use PALite to verify contents")
        super().setUp()

    def conn_config(self):
        return self.extensionsConfig() + ',disaggregated=(role="leader")'

    def conn_extensions(self, extlist):
        config = (f'=(early_load=true,config=\"verbose=-1,version={self.key_provider_version},'
                  f'key_expires={self.key_expires}\")')
        extlist.extension('test', "key_provider" + config)
        DisaggConfigMixin.conn_extensions(self, extlist)

    def generate_crypt_key(self, timestamp):
        # A unique key per push: the prefix followed by the timestamp.
        return self.KEY_PREFIX + str(timestamp).encode()

    def push_crypt_key(self, timestamp, key=None, conn=None):
        # Push a key at the given timestamp through the set_key API on the given connection.
        conn = conn or self.conn
        crypt = wiredtiger.CryptKeys()
        crypt.keys = self.generate_crypt_key(timestamp) if key is None else key
        crypt.timestamp = timestamp
        session = conn.open_session()
        self.assertEqual(conn.get_key_provider().set_key(session, crypt), 0)
        session.close()

    def sqlite_query(self, table, sql, home=None):
        # Use the bundled sqlite3 to match Palites SQLite version; some system builds are too old.
        home = self.home if home is None else home
        sqlite_exe = os.path.join(wt_builddir, 'sqlite3')
        database = os.path.join(home, 'kv_home', table)
        result = subprocess.run([sqlite_exe, '-json', database, sql],
            capture_output=True, text=True, check=True)
        return json.loads(result.stdout) if result.stdout.strip() else []

    def fetch_key_provider_pages(self, home=None):
        # Fetch every persisted KEK page, oldest first.
        rows = self.sqlite_query(self.key_provider_table,
            f'SELECT lsn, page_id, hex(page_data) AS hex FROM pages '
            f'WHERE table_id={self.WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID} ORDER BY lsn ASC;', home)
        pages = []
        for row in rows:
            data = bytes.fromhex(row['hex'])
            header_size = data[self.CRYPT_HEADER_SIZE_OFFSET]
            o = self.CRYPT_HEADER_TIMESTAMP_OFFSET
            pages.append({
                'lsn': row['lsn'],
                'page_id': row['page_id'],
                'data': data,
                'key': data[header_size:],
                'timestamp': int.from_bytes(data[o:o + 8], 'little'),
            })
        return pages

    def key_provider_page_count(self, home=None):
        return len(self.fetch_key_provider_pages(home))

    def validate_latest_kek(self, timestamp, home=None):
        # Validate the latest persisted page.
        pages = self.fetch_key_provider_pages(home)
        self.assertGreater(len(pages), 0)
        page = pages[-1]
        self.assertEqual(page['page_id'], self.MAIN_KEK_PAGE_ID)
        self.assertEqual(page['key'], self.generate_crypt_key(timestamp))
        self.assertEqual(page['timestamp'], timestamp)
        return page

    def validate_turtle_page(self, home=None):
        # The turtle's newest entry must reference the main KEK page at the turtle format version.
        # Returns the parsed match so callers can also read the LSN.
        rows = self.sqlite_query(self.turtle_table,
            f'SELECT * FROM pages WHERE table_id={self.WT_SPECIAL_PALI_TURTLE_FILE_ID} '
            f'ORDER BY lsn DESC LIMIT 1;', home)
        m = re.search(r"page_id=(?P<page_id>\d+),lsn=(?P<lsn>\d+).*version=(?P<version>\d+)",
            rows[0]['page_data'])
        self.assertTrue(m)
        self.assertEqual(int(m.group('page_id')), self.MAIN_KEK_PAGE_ID)
        self.assertEqual(int(m.group('version')), self.TURTLE_KEK_VERSION)
        return m
