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

# test_cursor_bound18.py
# Test the restoring of original primary bounds where setting bounds on some colgroup fails.
class test_cursor_bound18(bound_base):
    file_name = 'test_cursor_bound18'
    use_colgroup = True
    uri = 'table:'

    key_formats = [
        ('string', dict(key_format='S')),
        ('var', dict(key_format='r')),
        ('int', dict(key_format='i')),
        ('bytes', dict(key_format='u')),
        ('composite_string', dict(key_format='SSS')),
        ('composite_int_string', dict(key_format='iS')),
        ('composite_complex', dict(key_format='iSru')),
    ]

    value_formats = [
        ('string', dict(value_format='S')),
        ('complex-string', dict(value_format='SS')),
    ]

    config = [
        ('inclusive-evict', dict(lower_inclusive=True,upper_inclusive=True,evict=True)),
        ('no-inclusive-evict', dict(lower_inclusive=False,upper_inclusive=False,evict=True)),
        ('inclusive-no-evict', dict(lower_inclusive=True,upper_inclusive=True,evict=False)),
        ('no-inclusive-no-evict', dict(lower_inclusive=False,upper_inclusive=False,evict=False))
    ]

    direction = [
        ('prev', dict(next=False)),
        ('next', dict(next=True)),
    ]

    scenarios = make_scenarios(key_formats, value_formats, config, direction)

    def test_bound_api(self):
        cursor = self.create_session_and_cursor()                                                                                                                                                                                                                                        

        # Test bound api: Test default bound api with column groups.
        self.assertEqual(self.set_bounds(cursor, 40, "lower"), 0) 
        self.assertEqual(self.set_bounds(cursor, 90, "upper"), 0)

        # Test bound api: Test that original bounds persist even if setting one bound fails. 
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 30, "upper"), '/Invalid argument/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 95, "lower"), '/Invalid argument/')

        self.assertEqual(self.set_bounds(cursor, 50, "lower"), 0) 
        self.assertEqual(self.set_bounds(cursor, 80, "upper"), 0)
        self.cursor_traversal_bound(cursor, 50, 80, None)

        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 40, "upper"), '/Invalid argument/')
        cursor.set_key(self.gen_key(50))
        self.cursor_traversal_bound(cursor, 50, 90, None)

        self.assertEqual(self.set_bounds(cursor, 70, "upper"), 0) 
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 80, "lower"), '/Invalid argument/')
        self.cursor_traversal_bound(cursor, 50, 70, None)

        # Test bound api: Test successful setting of both bounds.
        self.assertEqual(self.set_bounds(cursor, 50, "lower"), 0) 
        self.assertEqual(self.set_bounds(cursor, 70, "upper"), 0) 
        self.cursor_traversal_bound(cursor, 50, 70, None)
