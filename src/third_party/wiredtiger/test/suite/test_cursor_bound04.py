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

# test_cursor_bound04.py
# Test the next() and prev() calls in the cursor bound API. There are two scenarios that are
# tested in this python test.
#   1. Test combination scenarios of using next() and prev() together.
#   2. Test clearing bounds and special scenarios of the cursor API usage.
class test_cursor_bound04(bound_base):
    file_name = 'test_cursor_bound04'

    types = [
        ('file', dict(uri='file:', use_colgroup=False)),
        ('table', dict(uri='table:', use_colgroup=False)),
        ('colgroup', dict(uri='table:', use_colgroup=True))
    ]

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
        ('evict', dict(evict=True)),
        ('no-evict', dict(evict=False))
    ]
    scenarios = make_scenarios(types, key_formats, value_formats, config)

    def test_bound_special_scenario(self):
        cursor = self.create_session_and_cursor()

        # Test bound api: Test lower bound clearing works.
        self.set_bounds(cursor, 45, "lower")
        cursor.bound("action=clear")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(self.start_key))
        cursor.reset()
        
        # Test bound api: Test lower bound setting with positioned cursor.
        self.set_bounds(cursor, 45, "lower")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(45))
        cursor.set_key(self.gen_key(40))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 50, "lower"), '/Invalid argument/')
        cursor.reset()

        self.set_bounds(cursor, 45, "lower")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(45))
        cursor.set_key(self.gen_key(90))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 50, "lower"), '/Invalid argument/')
        cursor.reset()

        self.set_bounds(cursor, 45, "lower")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(45))
        cursor.set_key(self.gen_key(10))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 50, "lower"), '/Invalid argument/')
        cursor.reset()

        # Test bound api: Test upper bound setting with positioned cursor.
        self.set_bounds(cursor, 55, "upper")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(self.start_key))
        cursor.set_key(self.gen_key(60))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 50, "upper"), '/Invalid argument/')
        cursor.reset()

        self.set_bounds(cursor, 55, "upper")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(self.start_key))
        cursor.set_key(self.gen_key(90))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 50, "upper"),
            '/Invalid argument/')
        cursor.reset()

        self.set_bounds(cursor, 55, "upper")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(self.start_key))
        cursor.set_key(self.gen_key(10))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 50, "upper"),
            '/Invalid argument/')
        cursor.reset()

        # Test bound api: Test inclusive lower bound setting with positioned cursor.
        self.set_bounds(cursor, 55, "lower")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(55))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=upper,inclusive=false"),
            '/Invalid argument/')
        cursor.reset()
        
        # Test bound api: Test inclusive upper bound setting with positioned cursor.
        self.set_bounds(cursor, 55, "upper")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(self.start_key))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=upper,inclusive=false"),
            '/Invalid argument/')
        cursor.reset()

        # Test bound api: Test clearing bounds on a positioned cursor.
        self.set_bounds(cursor, 55, "lower")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(55))
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.cursor_traversal_bound(cursor, None, None, True,  self.end_key - 55)
        cursor.reset()

        self.set_bounds(cursor, 55, "upper")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(self.start_key))
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.cursor_traversal_bound(cursor, None, None, True,  self.end_key - self.start_key)
        cursor.reset()
        cursor.close()

    def test_bound_combination_scenario(self):
        cursor = self.create_session_and_cursor()

        # Test basic cases of setting lower bound and performing combination of next() and
        # prev() calls.
        self.set_bounds(cursor, 45, "lower")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(45))
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(46))
        self.assertEqual(cursor.prev(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(45))
        self.assertEqual(cursor.prev(), wiredtiger.WT_NOTFOUND)

        # Test basic cases of setting upper bound and performing combination of next() and
        # prev() calls.
        self.set_bounds(cursor, 60, "upper")
        self.assertEqual(cursor.prev(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(60))
        self.assertEqual(cursor.prev(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(59))
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(60))
        self.assertEqual(cursor.next(), wiredtiger.WT_NOTFOUND)

        # Test bound api: Test that prev() works after next() traversal.
        self.set_bounds(cursor, 45, "lower")
        self.set_bounds(cursor, 50, "upper")
        self.cursor_traversal_bound(cursor, 45, 50, True)
        self.assertEqual(cursor.prev(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(50))  
        cursor.reset()

        # Test bound api: Test that next() works after prev() traversal.
        self.set_bounds(cursor, 45, "lower")
        self.set_bounds(cursor, 50, "upper")
        self.cursor_traversal_bound(cursor, 45, 50, False)
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(45)) 
        cursor.reset()

        # Test special case where we position with bounds then clear, then traverse opposite
        # direction.
        self.set_bounds(cursor, 45, "lower")
        self.assertEqual(cursor.next(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(45))
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.cursor_traversal_bound(cursor, None, None, False,  45 - self.start_key)
        cursor.reset()

        self.set_bounds(cursor, 45, "upper")
        self.assertEqual(cursor.prev(), 0)
        key = cursor.get_key()
        self.assertEqual(key, self.check_key(45))
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.cursor_traversal_bound(cursor, None, None, True,  self.end_key - 45)
        cursor.reset()

        cursor.close()

if __name__ == '__main__':
    wttest.run()
