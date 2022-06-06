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

# test_cursor_bound01.py
#    Basic cursor bound API validation.
class test_cursor_bound01(wttest.WiredTigerTestCase):
    file_name = 'test_cursor_bound01'

    types = [
        ('file', dict(uri='file:', use_index = False, use_colgroup = False)),
        ('table', dict(uri='table:', use_index = False, use_colgroup = False)),
        ('lsm', dict(uri='lsm:', use_index = False, use_colgroup = False)),
        ('colgroup', dict(uri='table:', use_index = False, use_colgroup = False)),
        #FIXME: Turn on once index cursor bound implementation is done.
        #('index', dict(uri='table:', use_index = True)), 
    ]
    scenarios = make_scenarios(types)

    def test_bound_api(self):
        uri = self.uri + self.file_name
        create_params = 'value_format=S,key_format=i'
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
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("bound=lower"),
                '/Unsupported cursor operation/')
            return

        # Cursor bound API should return EINVAL if no configurations are passed in.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound(),
            '/Invalid argument/')

        # Check that bound configuration works properly.
        cursor.set_key(0)
        cursor.bound("bound=lower")
        cursor.set_key(10)
        cursor.bound("bound=upper")

        # Clear and inclusive configuration are not compatible with each other. 
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("action=clear,inclusive=true"),
            '/Invalid argument/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda: cursor.bound("action=clear,inclusive=false"),
            '/Invalid argument/')

        # Check that clear with bound configuration works properly.
        cursor.bound("action=clear")
        cursor.bound("action=clear,bound=lower")
        cursor.bound("action=clear,bound=upper")

if __name__ == '__main__':
    wttest.run()
