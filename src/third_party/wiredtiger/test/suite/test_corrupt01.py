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

import random
import re
from suite_subprocess import suite_subprocess
import wttest
import wiredtiger

# test_corrupt01.py
# Test the verbose log messages in the event of a corrupted block. We should expect to see a dump
# of the block bytes along with a dump of each checkpoints extent lists.
# FIXME-WT-14867: Enable this test once disaggregated storage supports Verify.
@wttest.skip_for_hook("disagg", "Verify is not supported with disaggregated storage (yet)")
class test_corrupt01(wttest.WiredTigerTestCase, suite_subprocess):
    uri = 'table:test_corrupt01'
    conn_config = 'cache_size=100MB,statistics=(all),debug_mode=(corruption_abort=false)'
    num_kv = 10000
    table_config = 'key_format=i,value_format=S,allocation_size=512B,leaf_page_max=4KB'

    def remove(self):
        """
        Create holes in the file to ensure there are elements in the extent list.
        """
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.num_kv):
            if i % 2 == 0:
                cursor.set_key(i)
                cursor.remove()
        cursor.close()

    def insert(self):
        """
        Insert key value pairs with random size values. This helps create more fragmented blocks
        and therefore a larger extent list.
        """
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.num_kv):
            # Generate a random size value between of 1KB, 2KB, 3KB, or 4KB.
            value_size = random.randint(1, 4)
            value = 'a' * value_size * 1024
            cursor[i] = value
        cursor.close()

    def read_all(self):
        """
        Read all key value pairs to ensure we eventually read the corrupted block.
        """
        cursor = self.session.open_cursor(self.uri, None, None)
        while cursor.next() == 0:
            continue
        cursor.close()

    def test_corrupt01(self):
        # Create and populate the table.
        self.session.create(self.uri, self.table_config)
        self.insert()
        self.session.checkpoint()

        # Remove some keys to create holes in the file.
        self.remove()
        self.session.checkpoint()

        self.conn.close()

        # Dump the block information to a file.
        dump_file = 'dump_output.txt'
        self.runWt(
            ['verify', '-d', 'dump_address', self.uri],
            outfilename=dump_file,
            closeconn=False
        )

        # Read the dump file to find a used address to corrupt.
        addr_start = None
        with open(dump_file, 'r', encoding='utf-8') as f:
            for line in f:
                if 'row-store leaf' in line:
                    # Extract the address of the block:
                    # An example line looks like
                    # [0: 708608-737280, 28672, 2171724032] newest_durable: (0, 0)/(0, 0)...
                    # We are looking for the address in the format 708608-737280
                    addr_match = re.search(r'\[\d+: (\d+-\d+),', line)
                    # Pick the first address of a leaf page found in the dump output.
                    if addr_match:
                        self.pr('Address to corrupt: ' + addr_match.group(1))
                        addr_start = addr_match.group(1).split('-')[0]
                        break

        if addr_start is None:
            self.fail("No address found in dump output to corrupt.")

        # Corrupt the block by writing "BAD_VALUE" to the address.
        file_name = self.uri.split(':')[1] + '.wt'
        with open(file_name, 'r+b') as f:
            f.seek(int(addr_start))
            f.write(b'BAD_VALUE')

        try:
            # Reopen the connection to trigger the corruption.
            corrupt_conn = self.setUpConnectionOpen('.')
            self.session = self.setUpSessionOpen(corrupt_conn)
            self.read_all()
        except wiredtiger.WiredTigerError as e:
            pass
        finally:
            self.assertRaises(
                wiredtiger.WiredTigerError, lambda: corrupt_conn.close())

        self.ignoreStdoutPatternIfExists('extent list')
        self.ignoreStderrPatternIfExists('checksum error')
