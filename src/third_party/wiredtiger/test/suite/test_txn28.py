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

import wiredtiger, wttest
from wtdataset import SimpleDataSet
import os, re

# test_txn28.py
#   Test that checking the snapshot array is correctly outputted.
class test_txn28(wttest.WiredTigerTestCase):
    # Get the number after a substring in the string.
    def get_number_after_substring(self, text, sub_str):
        pattern = rf'{re.escape(sub_str)}\s*(\d+)'
        match = re.search(pattern, text)
        if match:
            return match.group(1)
        else:
            return None

    # Get the number of digits between two strings.
    def count_integers_between_substrings(self, text, sub1, sub2):
        # Regular expression matching integers.
        pattern = re.compile(r'\d+')
        # Find the position of the substring.
        start_index = text.find(sub1)
        end_index = text.find(sub2, start_index + len(sub1))
        # If the substring does not exist, return -1.
        if start_index == -1 or end_index == -1:
            return -1
        # Extract text between substrings.
        substring = text[start_index + len(sub1):end_index]
        # Use regular expressions to match integers and calculate quantities.
        return len(pattern.findall(substring))

    def test_snapshot_array_dump(self):
        uri = "table:txn28"
        # Create and populate a table.
        table_params = 'key_format=i,value_format=S'
        self.session.create(uri, table_params)
        cursor = self.session.open_cursor(uri, None)
        for i in range(1, 10):
            cursor[i] = str(i)
        cursor.close()

        # The first session.
        session1 = self.session
        cursor1 = session1.open_cursor(uri)
        session1.begin_transaction()
        cursor1[5] = "aaa"

        # The second session.
        session2 = self.conn.open_session()
        cursor2 = session2.open_cursor(uri)
        session2.begin_transaction()
        cursor2[6] = "bbb"

        # The third session.
        session3 = self.conn.open_session()
        cursor3 = session2.open_cursor(uri)
        session3.begin_transaction()
        cursor3[7] = "ccc"

        # Get the stdout.txt file directory.
        stdout_path = os.path.join(os.getcwd(), 'stdout.txt')
        max_snapshot_list_item_count = 0
        # Get the "transaction state dump" info, get content of " snapshot count: " and "snapshot: [xxx]"'s digital num.
        with self.expectedStdoutPattern('transaction state dump'):
            self.conn.debug_info('txn')

        with open(stdout_path, 'r') as file:
            for line in file:
                snapshot_count_str = ", snapshot count: "
                if snapshot_count_str in line:
                    snapshot_count = int(self.get_number_after_substring(line, snapshot_count_str))
                    sub1 = ", snapshot: ["
                    sub2 = "], commit_timestamp:"
                    snapshot_list_item_count = self.count_integers_between_substrings(line, sub1, sub2)
                    self.assertEqual(snapshot_count, snapshot_list_item_count)
                    if max_snapshot_list_item_count < snapshot_list_item_count:
                        max_snapshot_list_item_count = snapshot_list_item_count

        self.assertEqual(max_snapshot_list_item_count, 2)
        session3.commit_transaction()
        session2.commit_transaction()
        session1.commit_transaction()
