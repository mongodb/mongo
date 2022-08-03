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

# test_cursor_bound10.py
# Test next/prev history store scenarios with cursor bound API.
class test_cursor_bound10(bound_base):
    file_name = 'test_cursor_bound10'
    lower_inclusive = True
    upper_inclusive = True

    types = [
        ('file', dict(uri='file:', use_colgroup=False)),
        ('table', dict(uri='table:', use_colgroup=False)),
        ('colgroup', dict(uri='table:', use_colgroup=True))
    ]

    key_format_values = [
        ('var', dict(key_format='r')),
        ('int', dict(key_format='i')),
        ('composite_int_string', dict(key_format='iS')),
        ('composite_complex', dict(key_format='iSru')),
    ]

    config = [
        ('evict', dict(evict=True)),
        ('no-evict', dict(evict=False)),
    ]

    direction = [
        ('prev', dict(next=False)),
        ('next', dict(next=True)),
    ]

    scenarios = make_scenarios(types, key_format_values, config, direction)

    def create_session_and_cursor(self):
        uri = self.uri + self.file_name
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        self.session.create(uri, create_params)

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, 101):
            cursor[self.gen_key(i)] = "value" + str(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(50))

        self.session.begin_transaction()
        for i in range(101, 601):
            cursor[self.gen_key(i)] = "value" + str(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(200))

        self.session.begin_transaction()
        for i in range(601, 1001):
            cursor[self.gen_key(i)] = "value" + str(i)
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(100))

        if (self.evict):
            evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
            for i in range(1, 1001):
                evict_cursor.set_key(self.gen_key(i))
                evict_cursor.search()
                evict_cursor.reset() 
        return cursor
 
    def test_bound_general_scenario(self):
        cursor = self.create_session_and_cursor()

        # Test bound api: Test early exit works with upper bound.
        self.set_bounds(cursor, 900, "upper")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(10))
        self.cursor_traversal_bound(cursor, None, 900, self.next, 0)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 900, "upper")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(75))
        self.cursor_traversal_bound(cursor, None, 900, self.next, 100)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)
        
        self.set_bounds(cursor, 900, "upper")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(150))
        self.cursor_traversal_bound(cursor, None, 900, self.next, 400)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 900, "upper")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(250))
        self.cursor_traversal_bound(cursor, None, 900, self.next, 900)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal works with lower bound.
        self.set_bounds(cursor, 50, "lower")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(10))
        self.cursor_traversal_bound(cursor, 50, None, self.next, 0)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 50, "lower")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(75))
        self.cursor_traversal_bound(cursor, 50, None, self.next, 51)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)
        
        self.set_bounds(cursor, 50, "lower")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(150))
        self.cursor_traversal_bound(cursor, 50, None, self.next, 451)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 50, "lower")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(250))
        self.cursor_traversal_bound(cursor, 50, None, self.next, 951)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal with both bounds.
        self.set_bounds(cursor, 50, "lower")
        self.set_bounds(cursor, 900, "upper")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(10))
        self.cursor_traversal_bound(cursor, 50, 900, self.next, 0)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 50, "lower")
        self.set_bounds(cursor, 900, "upper")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(75))
        self.cursor_traversal_bound(cursor, 50, 900, self.next, 51)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)
        
        self.set_bounds(cursor, 50, "lower")
        self.set_bounds(cursor, 900, "upper")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(150))
        self.cursor_traversal_bound(cursor, 50, 900, self.next, 351)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)
        
        self.set_bounds(cursor, 50, "lower")
        self.set_bounds(cursor, 900, "upper")
        self.session.begin_transaction('read_timestamp=' +  self.timestamp_str(250))
        self.cursor_traversal_bound(cursor, 50, 900, self.next, 851)
        self.session.commit_transaction()
        self.assertEqual(cursor.bound("action=clear"), 0)
        cursor.close()

if __name__ == '__main__':
    wttest.run()
