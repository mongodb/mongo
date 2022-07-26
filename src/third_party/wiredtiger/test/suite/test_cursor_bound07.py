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

# test_cursor_bound07.py
# Test column store related scenarios with the bounds API. 
class test_cursor_bound07(bound_base):
    file_name = 'test_cursor_bound07'
    start_key = 10
    end_key = 100
    key_format = 'r'
    lower_inclusive = True
    upper_inclusive = True

    types = [
        ('file', dict(uri='file:')),
        ('table', dict(uri='table:')),
    ]
    
    evict = [
        ('evict', dict(evict=True)),
        ('no-evict', dict(evict=True)),
    ]

    record_values = [
        ('all-records-rle', dict(records_rle=True,deleted_rle=True)),
        ('only-deleted-records-rle', dict(records_rle=False,deleted_rle=True)),
        ('only-normal-records-rle', dict(records_rle=True,deleted_rle=False)),
        ('no-records-rle', dict(records_rle=False,deleted_rle=False)),
    ]

    direction = [
        ('prev', dict(next=False)),
        ('next', dict(next=True)),
    ]
    scenarios = make_scenarios(types, direction, evict, record_values)

    def create_session_and_cursor(self):
        uri = self.uri + self.file_name
        create_params = 'value_format=S,key_format={}'.format(self.key_format)    
        self.session.create(uri, create_params)

        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(10, 31):
            value = "value" + str(i) if not self.records_rle else "value"
            cursor[self.gen_key(i)] = value
        self.session.commit_transaction()

        self.session.begin_transaction()
        for i in range(71, 101):
            value = "value" + str(i) if not self.records_rle else "value"
            cursor[self.gen_key(i)] = value
        self.session.commit_transaction()
        
        self.session.begin_transaction()
        for i in range(31, 71):
            value = "value" + str(i) if not self.deleted_rle else "value"
            cursor[self.gen_key(i)] = value
            cursor.set_key(self.gen_key(i))
            self.assertEqual(cursor.remove(), 0)
        self.session.commit_transaction()

        if (self.evict):
            evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
            for i in range(self.start_key, self.end_key):
                evict_cursor.set_key(self.gen_key(i))
                evict_cursor.search()
                evict_cursor.reset() 
            evict_cursor.close()
        return cursor

    def test_bound_next_scenario(self):
        cursor = self.create_session_and_cursor()
    
        # Test bound api: Test early exit works with upper bound.
        self.set_bounds(cursor, 15, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 15, self.next)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal works with lower bound.
        self.set_bounds(cursor, 90, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, 90, None, self.next)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal with both bounds.
        self.set_bounds(cursor, 80, "lower", self.lower_inclusive)
        self.set_bounds(cursor, 90, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, 80, 90, self.next)
        self.assertEqual(cursor.bound("action=clear"), 0)
       
        # Test bound api: Test traversal with lower over deleted records.
        self.set_bounds(cursor, 50, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, 50, None, self.next, 29)
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 50, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 50, self.next, 20)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test column store insert list.
        self.session.begin_transaction()
        for i in range(51, 61):
            cursor[self.gen_key(i)] = "value" + str(i)
        self.session.commit_transaction()

        self.set_bounds(cursor, 55, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 55, self.next, 25)
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 55, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, 55, None, self.next, 35)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test inclusive option between a RLE record and a normal record.
        self.session.begin_transaction()
        cursor[101] = "value_normal" + str(i)
        self.session.commit_transaction()
        
        # FIX-ME-WT-9475: cursor_traversal_bound has a bug, therefore e-enable this check when the function is fixed.
        # self.set_bounds(cursor, 100, "lower", False)
        # self.cursor_traversal_bound(cursor, 100, None, self.next, 1)
        # self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 101, "upper", False)
        self.cursor_traversal_bound(cursor, None, 101, self.next, 60)
        self.assertEqual(cursor.bound("action=clear"), 0)

        self.session.begin_transaction()
        cursor[102] = "value_normal" + str(i)
        self.session.commit_transaction()

        # FIX-ME-WT-9475: cursor_traversal_bound has a bug, therefore e-enable this check when the function is fixed.
        # self.set_bounds(cursor, 100, "lower", False)
        # self.cursor_traversal_bound(cursor, 100, None, self.next, 2)
        # self.assertEqual(cursor.bound("action=clear"), 0)

        self.set_bounds(cursor, 101, "upper", False)
        self.cursor_traversal_bound(cursor, None, 101, self.next, 60)
        self.assertEqual(cursor.bound("action=clear"), 0)
        
        cursor.close()

if __name__ == '__main__':
    wttest.run()
