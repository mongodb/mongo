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
#
# Basic tests of the next raw key/value batch API.

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

class test_cursor27(wttest.WiredTigerTestCase):
    nentries = 5

    conn_config = 'statistics=(fast)'

    scenarios = make_scenarios([
        ('file', dict(uri='file:test_cursor27.wt')),
        ('table', dict(uri='table:test_cursor27')),
    ])

    def populate(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        cursor = self.session.open_cursor(self.uri)
        for i in range(self.nentries):
            cursor[f'k{i}'] = f'v{i}'
        cursor.close()

    def batch_stats(self):
        return (self.get_stat(wiredtiger.stat.conn.cursor_next_raw_kv_batch),
                self.get_stat(wiredtiger.stat.conn.cursor_next_raw_kv_batch_result))

    def test_walk(self):
        self.populate()
        cursor = self.session.open_cursor(self.uri)
        calls_before, results_before = self.batch_stats()

        rows = []
        calls = 0
        for _ in range(self.nentries):
            # The argument here is the maximum number of rows to return,
            # and the returned value is a list of (k, v) tuples where
            # both the key and value are byte strings.
            batch = cursor.next_raw_key_value_batch(3)
            calls += 1
            if not batch:
                break
            self.assertTrue(1 <= len(batch) <= 3)
            rows.extend(batch)

        calls_after, results_after = self.batch_stats()
        self.assertEqual(len(rows), self.nentries)
        self.assertStatEqualSoon(wiredtiger.stat.conn.cursor_next_raw_kv_batch, calls_before + calls)
        self.assertStatEqualSoon(wiredtiger.stat.conn.cursor_next_raw_kv_batch_result, results_before + len(rows))

        for i in range(self.nentries):
            key_raw, val_raw = rows[i]
            key = key_raw.rstrip(b'\x00').decode()
            val = val_raw.rstrip(b'\x00').decode()
            self.assertEqual(key, f'k{i}')
            self.assertEqual(val, f'v{i}')

    def test_position(self):
        self.populate()
        cursor = self.session.open_cursor(self.uri)

        batch = cursor.next_raw_key_value_batch(2)
        last = len(batch)
        key_raw, val_raw = batch[0]
        key = key_raw.rstrip(b'\x00').decode()
        val = val_raw.rstrip(b'\x00').decode()
        self.assertEqual(key, f'k{last - 1}')
        self.assertEqual(val, f'v{last - 1}')

        self.assertEqual(cursor.next(), 0)
        self.assertEqual(cursor.get_key(), f'k{last}')

    def test_invalid_count(self):
        self.populate()
        cursor = self.session.open_cursor(self.uri)
        for count in (0, -1):
            self.assertRaisesException(ValueError, lambda: cursor.next_raw_key_value_batch(count), '/positive integer/')
