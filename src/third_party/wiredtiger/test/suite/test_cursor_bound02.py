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

# test_cursor_bound02.py
#    Test that setting bounds of different key formats works in the cursor bound API. Make
# sure that WiredTiger complains when the upper and lower bounds overlap and that clearing the 
# bounds through the bound API and reset calls work appriopately.
class test_cursor_bound02(wttest.WiredTigerTestCase):
    file_name = 'test_cursor_bound02'

    types = [
        ('file', dict(uri='file:', use_colgroup=False)),
        ('table', dict(uri='table:', use_colgroup=False)),
        ('colgroup', dict(uri='table:', use_colgroup=True))
    ]

    key_format_values = [
        ('string', dict(key_format='S')),
        ('var', dict(key_format='r')),
        ('int', dict(key_format='i')),
        ('bytes', dict(key_format='u')),
        ('composite_string', dict(key_format='SSS')),
        ('composite_int_string', dict(key_format='iS')),
        ('composite_complex', dict(key_format='iSru')),
    ]

    inclusive = [
        ('inclusive', dict(inclusive=True)),
        ('no-inclusive', dict(inclusive=False))
    ]
    scenarios = make_scenarios(types, key_format_values, inclusive)
 
    def gen_key(self, i):
        tuple_key = []
        for key in self.key_format:
            if key == 'S' or key == 'u':
                tuple_key.append('key' + str(i))
            elif key == "r":
                tuple_key.append(self.recno(i))
            elif key == "i":
                tuple_key.append(i)
        
        if (len(self.key_format) == 1):
            return tuple_key[0]
        else:
            return tuple(tuple_key)

    def gen_colgroup_create_param(self):
        create_params = ",columns=("
        start = 0
        for _ in self.key_format:
            create_params += "k{0},".format(str(start)) 
            start += 1
        create_params += "v),colgroups=(g0)"
        return create_params

    def set_bounds(self, cursor, bound_config):
        inclusive_config = ",inclusive=false" if self.inclusive == False else ""
        return cursor.bound("bound={0}{1}".format(bound_config, inclusive_config))

    def test_bound_api(self):
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

        # Test bound API: Basic usage.
        cursor.set_key(self.gen_key(40))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0) 
        cursor.set_key(self.gen_key(90))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        # Test bound API: Upper bound < lower bound.
        cursor.set_key(self.gen_key(30))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, "upper"), '/Invalid argument/')

        # Test bound API: Lower bound > upper bound.
        cursor.set_key(self.gen_key(95))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, "lower"), '/Invalid argument/')

        # Test bound API: Test setting lower bound to 20, which would succeed setting upper
        # bound to 30
        cursor.set_key(self.gen_key(20))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(30))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)
    
        # Test bound API: Test setting upper bound to 99, which would succeed setting lower
        # bound to 90
        cursor.set_key(self.gen_key(99))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)
        cursor.set_key(self.gen_key(90))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)

        # Test bound API: No key set.
        cursor.reset()
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, "lower"), '/Invalid argument/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, "upper"), '/Invalid argument/')

        # Test bound API: Test that the key persists after lower bound call.
        cursor.set_key(self.gen_key(30))
        cursor.set_value("30")
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.insert()

        # Test bound API: Test that the key persists after upper bound call.
        cursor.set_key(self.gen_key(90))
        cursor.set_value("90")
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)
        cursor.insert()

        # Test bound API: that if lower bound is equal to the upper bound, that both bounds needs to
        # have inclusive configured. 
        cursor.bound("action=clear")
        cursor.set_key(self.gen_key(50))
        self.assertEqual(cursor.bound("bound=lower,inclusive=true"), 0)
        self.assertEqual(cursor.bound("bound=upper,inclusive=true"), 0)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=lower,inclusive=false"), '/Invalid argument/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=upper,inclusive=false"), '/Invalid argument/')
        
        # Test bound API: Test that only setting one of the bound inclusive config to true, should
        # fail too.
        cursor.bound("action=clear")
        self.assertEqual(cursor.bound("bound=lower,inclusive=false"), 0)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=upper,inclusive=false"), '/Invalid argument/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=upper,inclusive=true"), '/Invalid argument/')

        cursor.bound("action=clear")
        self.assertEqual(cursor.bound("bound=upper,inclusive=false"), 0)
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=lower,inclusive=false"), '/Invalid argument/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=lower,inclusive=true"), '/Invalid argument/')


    def test_bound_api_reset(self):
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

        cursor.set_key(self.gen_key(30))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(90))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        # Test bound API: Test that cursor reset works on the lower bound.
        cursor.set_key(self.gen_key(10))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, "upper"), '/Invalid argument/')
        cursor.reset()
        cursor.set_key(self.gen_key(10))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        # Test bound API: Test that cursor reset works on the upper bound.
        cursor.set_key(self.gen_key(99))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, "lower"), '/Invalid argument/')
        cursor.reset()
        cursor.set_key(self.gen_key(99))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
    
        # Test bound API: Test that cursor reset works the clearing bounds both ways. 
        cursor.set_key(self.gen_key(50))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.reset()
        cursor.set_key(self.gen_key(20))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(99))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        cursor.set_key(self.gen_key(55))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)
        cursor.reset()
        cursor.set_key(self.gen_key(90))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(99))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        # Test bound API: Make sure that a clear and reset works sequentially.
        cursor.reset()
        self.assertEqual(cursor.bound("action=clear"), 0)

        cursor.set_key(self.gen_key(30))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(90))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)
        self.assertEqual(cursor.bound("action=clear"), 0)
        cursor.reset()

        # Test bound API: Test that reset works after a reset.
        cursor.set_key(self.gen_key(30))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(90))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)
        cursor.reset()
        cursor.reset()

    def test_bound_api_clear(self):
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

        cursor.set_key(self.gen_key(30))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(90))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        # Test bound API: Test that clearing the lower bound works.
        cursor.set_key(self.gen_key(10))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, "upper"), '/Invalid argument/')
        self.assertEqual(cursor.bound("action=clear,bound=lower"), 0)
        cursor.set_key(self.gen_key(10))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        # Test bound API: Test that clearing the upper bound works. 
        cursor.set_key(self.gen_key(99))
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, "lower"), '/Invalid argument/')
        self.assertEqual(cursor.bound("action=clear,bound=upper"), 0)
        cursor.set_key(self.gen_key(99))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
    
        # Test bound API: Test that clearing both of the bounds works. 
        cursor.reset()
        cursor.set_key(self.gen_key(50))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)
        self.assertEqual(cursor.bound("action=clear"), 0)
        cursor.set_key(self.gen_key(90))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(99))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        cursor.reset()
        cursor.set_key(self.gen_key(50))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        self.assertEqual(cursor.bound("action=clear"), 0)
        cursor.set_key(self.gen_key(20))
        self.assertEqual(self.set_bounds(cursor, "lower"), 0)
        cursor.set_key(self.gen_key(99))
        self.assertEqual(self.set_bounds(cursor, "upper"), 0)

        # Test bound API: Test that clear works after a clear.
        self.assertEqual(cursor.bound("action=clear"), 0)
        self.assertEqual(cursor.bound("action=clear"), 0)

if __name__ == '__main__':
    wttest.run()
