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
from wtscenario import make_scenarios
from wtbound import bound_base

# test_cursor_bound05.py
# Test special scenario with cursor bound API. Make sure that internal cursor search properly 
# positions the cursor with bounds set as the prefix of the records.
class test_cursor_bound05(bound_base):
    file_name = 'test_cursor_bound05'
    key_format = 'S'
    start_key = 1000
    end_key = 2000

    types = [
        ('file', dict(uri='file:', use_colgroup=False)),
        ('table', dict(uri='table:', use_colgroup=False))
    ]

    config = [
        ('evict', dict(evict=True)),
        ('no-evict', dict(evict=False))
    ]
    scenarios = make_scenarios(types, config)

    def create_session_and_cursor(self):
        uri = self.uri + self.file_name
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        self.session.create(uri, create_params)

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(self.start_key, self.end_key + 1):
            cursor[self.gen_key(i)] = "value" + str(i)
        self.session.commit_transaction()

        if (self.evict):
            evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
            for i in range(self.start_key, self.end_key):
                evict_cursor.set_key(self.gen_key(i))
                evict_cursor.search()
                evict_cursor.reset() 
        return cursor

    def test_bound_special_scenario(self):
        cursor = self.create_session_and_cursor()

        # Test bound api: Test prefix key with lower bound.
        self.set_bounds(cursor, 10, "lower", True)
        self.cursor_traversal_bound(cursor, 10, None, True)
        self.cursor_traversal_bound(cursor, 10, None, False)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test prefix key with upper bound.
        self.set_bounds(cursor, 20, "upper", False)
        self.cursor_traversal_bound(cursor, None, 20, True, 999)
        self.cursor_traversal_bound(cursor, None, 20, False, 999)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test prefix key works with both bounds.
        self.set_bounds(cursor, 10, "lower", True)
        self.set_bounds(cursor, 20, "upper", False)
        self.cursor_traversal_bound(cursor, 10, 20, True, 999)
        self.cursor_traversal_bound(cursor, 10, 20, False, 999)
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 10, "lower", True)
        self.set_bounds(cursor, 11, "upper", False)
        self.cursor_traversal_bound(cursor, 10, 11, True, 99)
        self.cursor_traversal_bound(cursor, 10, 11, False, 99)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test early exit works with lower bound (Greater than data range).
        self.set_bounds(cursor, 30, "lower", True)
        self.cursor_traversal_bound(cursor, 30, None, True, 0)
        self.cursor_traversal_bound(cursor, 30, None, False, 0)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test early exit works with upper bound (Greater than data range).
        self.set_bounds(cursor, 40, "upper", False)
        self.cursor_traversal_bound(cursor, None, 40, True, 1000)
        self.cursor_traversal_bound(cursor, None, 40, False, 1000)
        self.assertEqual(cursor.bound("action=clear"), 0)

        cursor.close()

if __name__ == '__main__':
    wttest.run()
