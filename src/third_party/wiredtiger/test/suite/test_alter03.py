#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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

# test_alter03.py
#    Check if app_metadata can be altered.
class test_alter03(wttest.WiredTigerTestCase):
    name = "alter03"

    def verify_metadata(self, metastr):
        if metastr == '':
            return
        cursor = self.session.open_cursor('metadata:', None, None)
        #
        # Walk through all the metadata looking for the entries that are
        # the URIs for the named object.
        #
        found = False
        while True:
            ret = cursor.next()
            if ret != 0:
                break
            key = cursor.get_key()
            if key.find(self.name) != -1:
                value = cursor[key]
                found = True
                self.assertTrue(value.find(metastr) != -1)
        cursor.close()
        self.assertTrue(found == True)

    # Alter: Change the app_metadata and verify
    def test_alter03_app_metadata(self):
        uri = "table:" + self.name
        entries = 100
        create_params = 'key_format=i,value_format=i,'
        app_meta_orig = 'app_metadata="meta_data_1",'
        app_meta_new = 'app_metadata="meta_data_2",'

        self.session.create(uri, create_params + app_meta_orig)

        # Put some data in table.
        c = self.session.open_cursor(uri, None)
        for k in range(entries):
            c[k+1] = 1
        c.close()

        # Verify the string in the metadata
        self.verify_metadata(app_meta_orig)

        # Alter app metadata and verify
        self.session.alter(uri, app_meta_new)
        self.verify_metadata(app_meta_new)

if __name__ == '__main__':
    wttest.run()
