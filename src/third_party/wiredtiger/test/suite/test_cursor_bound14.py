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

# test_cursor_bound14.py
# Test write operation calls on a bounded cursor. Test general use cases of bound API,
# including setting lower bounds and upper bounds.
class test_cursor_bound14(bound_base):
    file_name = 'test_cursor_bound14'

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
        # FIX-ME-WT-9589: Fix bug complex colgroups not returning records within bounds.
        # ('complex-string', dict(value_format='SS')),
    ]

    cursor_config = [
        ('overwrite', dict(cursor_config="")),
        ('no-overwrite', dict(cursor_config="overwrite=false")),
    ]
    config = [
        ('lower-bounds-evict', dict(lower_bounds=True,upper_bounds=False,evict=True)),
        ('upper-bounds-evict', dict(lower_bounds=False,upper_bounds=True,evict=True)),
        ('both-bounds-evict', dict(lower_bounds=True,upper_bounds=True,evict=True)),
        ('lower-bounds-no-evict', dict(lower_bounds=True,upper_bounds=False,evict=True)),
        ('upper-bounds-no-evict', dict(lower_bounds=False,upper_bounds=True,evict=True)),
        ('both-bounds-no-evict', dict(lower_bounds=True,upper_bounds=True,evict=False)),
    ]

    scenarios = make_scenarios(types, key_formats, value_formats, config, cursor_config)

    def test_bound_data_operations(self):
        cursor = self.create_session_and_cursor(self.cursor_config)

        cursor.set_key(self.gen_key(10))
        cursor.set_value(self.gen_val(100))
        self.assertEqual(cursor.insert(), 0)

        cursor.set_key(self.gen_key(95))
        cursor.set_value(self.gen_val(100))
        self.assertEqual(cursor.insert(), 0)
        
        if (self.lower_bounds):
            self.set_bounds(cursor, 45, "lower", self.lower_inclusive)
        if (self.upper_bounds):
            self.set_bounds(cursor, 50, "upper", self.upper_inclusive)

        # Test bound API: test inserting key outside of bounds.
        cursor.set_key(self.gen_key(15))
        cursor.set_value(self.gen_val(120))
        if (self.lower_bounds):
            self.assertRaisesHavingMessage(
                wiredtiger.WiredTigerError, lambda: cursor.insert(), '/item not found/')
        else:
            self.assertEqual(cursor.insert(), 0)

        cursor.set_key(self.gen_key(90))
        cursor.set_value(self.gen_val(120))
        if (self.upper_bounds):
            self.assertRaisesHavingMessage(
                wiredtiger.WiredTigerError, lambda: cursor.insert(), '/item not found/')
        else:
            self.assertEqual(cursor.insert(), 0)

        # Test bound API: test updating an existing key outside of bounds.
        cursor.set_key(self.gen_key(10))
        cursor.set_value(self.gen_val(120))
        if (self.lower_bounds):
            self.assertEqual(cursor.update(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.update(), 0)
        

        cursor.set_key(self.gen_key(95))
        cursor.set_value(self.gen_val(120))
        if (self.upper_bounds):
            self.assertEqual(cursor.update(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.update(), 0)

        # Test bound API: test reserve on an existing key with bounds.
        self.session.begin_transaction()
        cursor.set_key(self.gen_key(10))
        if (self.lower_bounds):
            self.assertRaisesHavingMessage(
                wiredtiger.WiredTigerError, lambda: cursor.reserve(), '/item not found/')
        else:
            self.assertEqual(cursor.reserve(), 0)

        cursor.set_key(self.gen_key(95))
        if (self.upper_bounds):
            self.assertRaisesHavingMessage(
                wiredtiger.WiredTigerError, lambda: cursor.reserve(), '/item not found/')
        else:
            self.assertEqual(cursor.reserve(), 0)
        self.session.commit_transaction()

        # Test bound API: test modifies on an existing key outside of bounds.
        if (not self.use_colgroup):
            self.session.begin_transaction()
            cursor.set_key(self.gen_key(10))
            mods = [wiredtiger.Modify("2", 0, 1)]
            if (self.lower_bounds):
                self.assertEqual(cursor.modify(mods), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(cursor.modify(mods), 0)

            cursor.set_key(self.gen_key(95))
            mods = [wiredtiger.Modify("2", 0, 1)]
            if (self.upper_bounds):
                self.assertEqual(cursor.modify(mods), wiredtiger.WT_NOTFOUND)
            else:
                self.assertEqual(cursor.modify(mods), 0)
            self.session.commit_transaction()
        
        # Test bound API: test removing on an existing key outside of bounds.
        cursor.set_key(self.gen_key(10))
        if (self.lower_bounds):
            self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.remove(), 0)

        cursor.set_key(self.gen_key(95))
        if (self.upper_bounds):
            self.assertEqual(cursor.remove(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.remove(), 0)

        # Test update a key on the boundary of bounds.
        cursor.set_key(self.gen_key(45))
        cursor.set_value(self.gen_val(120))
        if (self.lower_bounds):
            if (self.lower_inclusive):
                self.assertEqual(cursor.update(), 0)
            else:
                self.assertEqual(cursor.update(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.update(), 0)

        cursor.set_key(self.gen_key(50))
        cursor.set_value(self.gen_val(120))
        if (self.upper_bounds):
            if (self.upper_inclusive):
                self.assertEqual(cursor.update(), 0)
            else:
                self.assertEqual(cursor.update(), wiredtiger.WT_NOTFOUND)
        else:
            self.assertEqual(cursor.update(), 0)
        cursor.close()

if __name__ == '__main__':
    wttest.run()
