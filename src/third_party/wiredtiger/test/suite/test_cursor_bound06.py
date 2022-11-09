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

# test_cursor_bound06.py
# Test the search() call in the cursor bound API.
class test_cursor_bound06(bound_base):
    file_name = 'test_cursor_bound06'

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

    inclusive = [
        ('inclusive', dict(inclusive=True)),
        ('no-inclusive', dict(inclusive=False))
    ]

    config = [
        ('no-evict', dict(evict=False)),
        ('evict', dict(evict=True))
    ]
    scenarios = make_scenarios(types, key_formats, value_formats, inclusive, config)

    def test_bound_search_scenario(self):
        cursor = self.create_session_and_cursor()
    
        # Test bound API: Search for a non-existent key with no bounds.
        cursor.set_key(self.gen_key(10))
        ret = cursor.search()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.reset()

        # Test bound API: Search for an existing key with no bounds.
        cursor.set_key(self.gen_key(50))
        ret = cursor.search()
        self.assertEqual(ret, 0)
        cursor.reset()

        # Test bound API: Search for a key outside of lower bound.
        self.set_bounds(cursor, 30, "lower", self.inclusive)
        cursor.set_key(self.gen_key(20))
        ret = cursor.search()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.reset()

        # Search for a key outside of upper bound.
        self.set_bounds(cursor, 40, "upper", self.inclusive)
        cursor.set_key(self.gen_key(60))
        ret = cursor.search()
        self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.reset()

        # Test bound API: Search for a valid key in the middle of the range.
        self.set_bounds(cursor, 20, "lower", self.inclusive)
        self.set_bounds(cursor, 40, "upper", self.inclusive)
        cursor.set_key(self.gen_key(35))
        ret = cursor.search()
        self.assertEqual(ret, 0)
        cursor.reset()

        # Test bound API: Search for a key next to the lower bound.
        self.set_bounds(cursor, 20, "lower", self.inclusive)
        cursor.set_key(self.gen_key(21))
        ret = cursor.search()
        self.assertEqual(ret, 0)
        cursor.reset()

        # Test bound API: Search for a key next to the upper bound.
        self.set_bounds(cursor, 40, "upper", self.inclusive)
        cursor.set_key(self.gen_key(39))
        ret = cursor.search()
        self.assertEqual(ret, 0)
        cursor.reset()

        # Test bound API: Search for a key equal to the bound. 
        self.set_bounds(cursor, 60, "upper", self.inclusive)
        cursor.set_key(self.gen_key(60))
        ret = cursor.search()
        if(self.inclusive):
            self.assertEqual(ret, 0)
        else:
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)
        cursor.reset()

        # Test bound API: Search for a key equal to the bound. 
        self.set_bounds(cursor, 20, "upper", self.inclusive)
        cursor.set_key(self.gen_key(20))
        ret = cursor.search()
        if(self.inclusive):
            self.assertEqual(ret, 0)
        else:
            self.assertEqual(ret, wiredtiger.WT_NOTFOUND)        
        cursor.reset()
        cursor.close()

if __name__ == '__main__':
    wttest.run()
