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

# test_cursor_bound01.py
#    Basic cursor bound API validation.
class test_cursor_bound01(bound_base):
    file_name = 'test_cursor_bound01'

    types = [
        ('file', dict(uri='file:', use_index = False, use_colgroup = False)),
        ('table', dict(uri='table:', use_index = False, use_colgroup = False)),
        ('lsm', dict(uri='lsm:', use_index = False, use_colgroup = False)),
        ('colgroup', dict(uri='table:', use_index = False, use_colgroup = False)),
        ('index', dict(uri='table:', use_index = True, use_colgroup = False)), 
    ]

    format_values = [
        ('string', dict(key_format='S',value_format='S')),
        ('var', dict(key_format='r',value_format='S')),
        ('fix', dict(key_format='r',value_format='8t'))
    ]

    scenarios = make_scenarios(types,format_values)

    def test_bound_api(self):
        # LSM doesn't support column store type, therefore we can just return early here.
        if (self.key_format == 'r' and self.uri == 'lsm:'):
            return

        uri = self.uri + self.file_name
        create_params = 'value_format={},key_format={}'.format(self.value_format, self.key_format)
        if self.use_index or self.use_colgroup:
            create_params += ",columns=(k,v)"
        if self.use_colgroup:
            create_params += ',colgroups=(g0)'
        self.session.create(uri, create_params)
        # Add in column group.
        if self.use_colgroup:
            create_params = 'columns=(v),'
            suburi = 'colgroup:table0:g0'
            self.session.create(suburi, create_params)

        cursor = None
        if self.use_index:
            # Test Index Cursors bound API support.
            suburi = "index:" + self.file_name + ":i0"
            self.session.create(suburi, "columns=(v)")
            cursor = self.session.open_cursor("index:" + self.file_name + ":i0")
        else:
            cursor = self.session.open_cursor(uri)

        # LSM format is not supported with range cursors.
        if self.uri == 'lsm:': 
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("action=set,bound=lower"),
                '/Operation not supported/')
            return
        if self.value_format == '8t':
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("action=set,bound=lower"),
                '/Invalid argument/')
            return

        # Cursor bound API should return EINVAL if no configurations are passed in.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound(),
            '/Invalid argument/')

        # Check that bound configuration works properly.
        if (self.use_index):
            cursor.set_key(self.gen_val(1))
            cursor.bound("bound=lower")
            cursor.set_key(self.gen_val(10))
            cursor.bound("bound=upper")
        else:
            cursor.set_key(self.gen_key(1))
            cursor.bound("bound=lower")
            cursor.set_key(self.gen_key(10))
            cursor.bound("bound=upper")

        # Check that clear works properly.
        cursor.bound("action=clear")

        # Index cursors work slightly differently to other cursors, we can early exit here as the
        # below edge cases don't apply for index cursors.
        if (self.use_index):
            return

        # Check that largest key doesn't work with bounded cursors.
        cursor.set_key(self.gen_key(1))
        cursor.bound("action=set,bound=lower")
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.largest_key(),
            '/setting bounds is not compatible with cursor largest key/')

        # Check edge cases with bounds config
        cursor.set_key(self.gen_key(1))
        # Setting the bound without providing an action works.
        self.assertEqual(cursor.bound("bound=lower"), 0)
        cursor.reset()

        cursor.set_key(self.gen_key(1))
        # Setting an action without a bound won't work.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("action=set"),
            '/a bound must be specified when setting bounds, either "lower" or "upper"/')
        cursor.reset()

        cursor.set_key(self.gen_key(1))
        # Giving a longer config string works, WT_PREFIX_MATCH will accept it.
        self.assertEqual(cursor.bound("action=setting, bound=lower"), 0)
        cursor.reset()

        cursor.set_key(self.gen_key(1))
        cursor.bound("action=set,bound=lower")
        # Giving a longer config string works, WT_PREFIX_MATCH will accept it.
        self.assertEqual(cursor.bound("action=clearing"), 0)
        cursor.reset()

        cursor.set_key(self.gen_key(1))
        # Giving an invalid action like "dump" won't work.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("action=dump"),
            '/an action of either "clear" or "set" should be specified when setting bounds/')
        cursor.reset()

        cursor.set_key(self.gen_key(1))
        # Giving a substring of the config string will not work.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("action=cl"),
            '/an action of either "clear" or "set" should be specified when setting bounds/')

        # Check that setting bounds doesn't work with random cursors. Turn it off with column store as column
        # store doesn't support the next_random config.
        if (self.key_format != 'r'):
            cursor = self.session.open_cursor(uri, None, "next_random=true")
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 40, "lower"), 
                '/Operation not supported/')
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: self.set_bounds(cursor, 60, "upper"), 
                '/Operation not supported/')

if __name__ == '__main__':
    wttest.run()
