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

# test_cursor_bound03.py
# Test the next() and prev() calls in the cursor bound API. Test general use cases of bound API,
# including setting lower bounds and upper bounds.
class test_cursor_bound03(bound_base):
    file_name = 'test_cursor_bound03'

    types = [
        ('file', dict(uri='file:', use_colgroup=False)),
        ('table', dict(uri='table:', use_colgroup=False)),
        ('colgroup', dict(uri='table:', use_colgroup=True))
    ]

    key_format_values = [
        ('string', dict(key_format='S')),
        # FIXME-WT-9474: Uncomment once column store is implemented.
        # ('var', dict(key_format='r')),
        ('int', dict(key_format='i')),
        ('bytes', dict(key_format='u')),
        ('composite_string', dict(key_format='SSS')),
        ('composite_int_string', dict(key_format='iS')),
        ('composite_complex', dict(key_format='iSru')),
    ]

    config = [
        ('inclusive-evict', dict(lower_inclusive=True,upper_inclusive=True,evict=True)),
        ('no-inclusive-evict', dict(lower_inclusive=False,upper_inclusive=False,evict=True)),
        ('lower-inclusive-evict', dict(lower_inclusive=True,upper_inclusive=False,evict=True)),
        ('upper-inclusive-evict', dict(lower_inclusive=False,upper_inclusive=True,evict=True)),
        ('inclusive-no-evict', dict(lower_inclusive=True,upper_inclusive=True,evict=False)),
        ('lower-inclusive-no-evict', dict(lower_inclusive=True,upper_inclusive=False,evict=False)),
        ('upper-inclusive-no-evict', dict(lower_inclusive=False,upper_inclusive=True,evict=False)),
        ('no-inclusive-no-evict', dict(lower_inclusive=False,upper_inclusive=False,evict=False))
    ]

    direction = [
        ('prev', dict(next=False)),
        ('next', dict(next=True)),
    ]

    scenarios = make_scenarios(types, key_format_values, config, direction)

    def create_session_and_cursor(self):
        uri = self.uri + self.file_name
        create_params = 'value_format=S,key_format={}'.format(self.key_format)
        if self.use_colgroup:
            create_params += self.gen_colgroup_create_param()
        self.session.create(uri, create_params)
        # Add in column group.
        if self.use_colgroup:
            create_params = 'columns=(v),'
            suburi = 'colgroup:{0}:g0'.format(self.file_name)
            self.session.create(suburi, create_params)

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

    def test_bound_general_scenario(self):
        cursor = self.create_session_and_cursor()

        # Test bound api: Test early exit works with upper bound.
        self.set_bounds(cursor, 50, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 50)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal works with lower bound.
        self.set_bounds(cursor, 45, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, 45, None)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal with both bounds.
        self.set_bounds(cursor, 45, "lower", self.lower_inclusive)
        self.set_bounds(cursor, 50, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, 45, 50)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal with lower bound (out of data range)
        self.set_bounds(cursor, self.start_key - 5, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, self.start_key - 5, None)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal with upper bound (out of data range).
        self.set_bounds(cursor, 95, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 95)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal with both bounds (out of data range).
        self.set_bounds(cursor, 10, "lower", self.lower_inclusive)
        self.set_bounds(cursor, 95, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, 10, 95)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test traversal with both bounds with no data in range.
        self.set_bounds(cursor, 95, "lower", self.lower_inclusive)
        self.set_bounds(cursor, 99, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, 95, 99, self.direction, 0)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test that clearing bounds works.
        self.set_bounds(cursor, 45, "lower", self.lower_inclusive)
        self.set_bounds(cursor, 50, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, 45, 50)
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.cursor_traversal_bound(cursor, None, None, True)
        self.assertEqual(cursor.reset(), 0)

        # Test bound api: Test upper bound clearing with only lower bounds.
        self.set_bounds(cursor, 50, "lower")
        cursor.bound("action=clear,bound=upper")
        self.cursor_traversal_bound(cursor, None, None, self.direction, self.end_key - 50)

        cursor.bound("action=clear,bound=lower")
        self.cursor_traversal_bound(cursor, None, None)
        
        # Test bound api: Test that changing upper bounds works.
        self.set_bounds(cursor, 50, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 50)
        self.set_bounds(cursor, 55, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 55)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test that changing upper bounds works (out of data range).
        self.set_bounds(cursor, 50, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 50)
        self.set_bounds(cursor, 95, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 95)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test that changing upper bounds works into data range.
        self.set_bounds(cursor, 95, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 95)
        self.set_bounds(cursor, 50, "upper", self.upper_inclusive)
        self.cursor_traversal_bound(cursor, None, 50)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test that changing lower bounds works.
        self.set_bounds(cursor, 50, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, 50, None)
        self.set_bounds(cursor, 45, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, 45, None)
        self.assertEqual(cursor.bound("action=clear"), 0)

        # Test bound api: Test that changing lower bounds works (out of data range).
        self.set_bounds(cursor, 50, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, 50, None)
        self.set_bounds(cursor, 15, "lower", self.lower_inclusive)
        self.cursor_traversal_bound(cursor, 15, None)
        self.assertEqual(cursor.bound("action=clear"), 0)

        cursor.close()

if __name__ == '__main__':
    wttest.run()
