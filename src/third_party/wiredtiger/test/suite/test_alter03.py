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
from helper_tiered import TieredConfigMixin, tiered_storage_sources
from wtscenario import make_scenarios

# test_alter03.py
#    Check if app_metadata can be altered.
class test_alter03(TieredConfigMixin, wttest.WiredTigerTestCase):
    name = "alter03"

    # Build all scenarios
    scenarios = make_scenarios(tiered_storage_sources)

    def verify_metadata(self, table_metastr, lsm_metastr, file_metastr):
        c = self.session.open_cursor('metadata:', None, None)

        if table_metastr != '':
            # We must find a table type entry for this object and its value
            # should contain the provided table meta string.
            c.set_key('table:' + self.name)
            self.assertNotEqual(c.search(), wiredtiger.WT_NOTFOUND)
            value = c.get_value()
            self.assertTrue(value.find(table_metastr) != -1)

        if lsm_metastr != '':
            # We must find a lsm type entry for this object and its value
            # should contain the provided lsm meta string.
            c.set_key('lsm:' + self.name)
            self.assertNotEqual(c.search(), wiredtiger.WT_NOTFOUND)
            value = c.get_value()
            self.assertTrue(value.find(lsm_metastr) != -1)

        if file_metastr != '':
            # We must find a file type entry for the object and its value
            # should contain the provided file meta string.
            if self.is_tiered_scenario():
                c.set_key('file:' + self.name + '-0000000001.wtobj')
                
                # Removing quotes wrapping app metadata value just to make the test pass.
                # FIXME: WT-9036
                file_metastr = 'app_metadata=meta_data_1,'
            else:
                c.set_key('file:' + self.name + '.wt')

            self.assertNotEqual(c.search(), wiredtiger.WT_NOTFOUND)
            value = c.get_value()
            self.assertTrue(value.find(file_metastr) != -1)

        c.close()

    # Alter Table: Change the app_metadata and verify
    def test_alter03_table_app_metadata(self):
        uri = "table:" + self.name
        entries = 100
        create_params = 'key_format=i,value_format=i,'
        app_meta_orig = 'app_metadata="meta_data_1",'

        self.session.create(uri, create_params + app_meta_orig)

        # Put some data in table.
        c = self.session.open_cursor(uri, None)
        for k in range(entries):
            c[k+1] = 1
        c.close()

        # Verify the string in the metadata
        self.verify_metadata(app_meta_orig, '', app_meta_orig)

        # Alter app metadata and verify
        self.alter(uri, 'app_metadata="meta_data_2",')
        self.verify_metadata('app_metadata="meta_data_2",', '', 'app_metadata="meta_data_2",')

        # Alter app metadata, explicitly asking for exclusive access and verify
        self.alter(uri, 'app_metadata="meta_data_3",exclusive_refreshed=true,')
        self.verify_metadata('app_metadata="meta_data_3",', '', 'app_metadata="meta_data_3",')

        # Alter app metadata without taking exclusive lock and verify that only
        # table object gets modified
        self.alter(uri, 'app_metadata="meta_data_4",exclusive_refreshed=false,')
        self.verify_metadata('app_metadata="meta_data_4",', '', 'app_metadata="meta_data_3",')

        # Open a cursor, insert some data and try to alter with session open.
        # We should fail unless we ask not to take an exclusive lock
        c2 = self.session.open_cursor(uri, None)
        for k in range(entries):
            c2[k+1] = 2

        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.alter(uri, 'app_metadata="meta_data_5",'))
        self.verify_metadata('app_metadata="meta_data_4",', '', 'app_metadata="meta_data_3",')

        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.alter(uri,
                'exclusive_refreshed=true,app_metadata="meta_data_5",'))
        self.verify_metadata('app_metadata="meta_data_4",', '', 'app_metadata="meta_data_3",')

        self.alter(uri, 'app_metadata="meta_data_5",exclusive_refreshed=false,')
        self.verify_metadata('app_metadata="meta_data_5",', '', 'app_metadata="meta_data_3",')

        c2.close()

        # Close and reopen the connection.
        # Confirm we retain the app_metadata as expected after reopen
        self.reopen_conn()
        self.verify_metadata('app_metadata="meta_data_5",', '', 'app_metadata="meta_data_3",')

    # Alter LSM: A non exclusive alter should not be allowed
    def test_alter03_lsm_app_metadata(self):
        if self.is_tiered_scenario():
            self.skipTest('Tiered storage does not support LSM.')
        
        uri = "lsm:" + self.name
        create_params = 'key_format=i,value_format=i,'
        app_meta_orig = 'app_metadata="meta_data_1",'

        self.session.create(uri, create_params + app_meta_orig)

        # Try to alter app metadata without exclusive access and verify
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.alter(uri,
                'exclusive_refreshed=false,app_metadata="meta_data_2",'),
                '/is applicable only on simple tables/')
        self.verify_metadata('', 'app_metadata="meta_data_1",', '')

        # Alter app metadata, explicitly asking for exclusive access and verify
        self.alter(uri, 'exclusive_refreshed=true,app_metadata="meta_data_2",')
        self.verify_metadata('', 'app_metadata="meta_data_2",', '')

        # Alter app metadata and verify
        self.alter(uri, 'app_metadata="meta_data_3",')
        self.verify_metadata('', 'app_metadata="meta_data_3",', '')

if __name__ == '__main__':
    wttest.run()
