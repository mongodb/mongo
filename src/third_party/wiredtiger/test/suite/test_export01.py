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
#
# test_export01.py
#   Tests basic export functionality.
#   - Test that the WiredTiger.export file is correctly created and removed.
#   - Test that the WiredTiger.export file contains the correct contents
#     after performing operations on the home directory, creating a backup
#     directory, and then performing operation on the backup directory.

from helper import copy_wiredtiger_home
from helper_tiered import TieredConfigMixin, gen_tiered_storage_sources
from wtscenario import make_scenarios
import os, shutil, wttest

class test_export01(TieredConfigMixin, wttest.WiredTigerTestCase):
    dir = 'backup.dir'

    types = [
        ('table', dict(type = 'table:')),
    ]
    tiered_storage_sources = gen_tiered_storage_sources()

    scenarios = make_scenarios(tiered_storage_sources, types)

    def test_export(self):
        uri_a = self.type + "exporta"
        uri_b = self.type + "exportb"
        uri_c = self.type + "exportc"

        # Create a few tables.
        self.session.create(uri_a)
        self.session.create(uri_b)
        self.session.create(uri_c)

        # Insert some records.
        c1 = self.session.open_cursor(uri_a)
        c1["k1"] = "v1"
        c1.close()

        c2 = self.session.open_cursor(uri_b)
        c2["k2"] = "v2"
        c2.close()

        c3 = self.session.open_cursor(uri_c)
        c3["k3"] = "v3"
        c3.close()

        self.session.checkpoint()

        if self.is_tiered_scenario():
            self.session.checkpoint('flush_tier=(enabled)')

        # Open a special backup cursor for export operation.
        export_cursor = self.session.open_cursor('backup:export', None, None)

        os.mkdir(self.dir)
        copy_wiredtiger_home(self, '.', self.dir)
        self.assertTrue(os.path.isfile("WiredTiger.export"))

        export_cursor.close()

        # The export file should be removed from the home directory.
        self.assertFalse(os.path.isfile("WiredTiger.export"))
        
        # The export file should exist in the backup directory.
        self.assertTrue(os.path.isfile(os.path.join(self.dir, "WiredTiger.export")))

    def test_export_restart(self):
        uri_a = self.type + "exporta"
        uri_b = self.type + "exportb"
        uri_c = self.type + "exportc"

        # Create two tables.
        self.session.create(uri_a)
        self.session.create(uri_b)

        # Insert some records.
        c4 = self.session.open_cursor(uri_a)
        c4["k4"] = "v4"
        c4.close()

        c5 = self.session.open_cursor(uri_b)
        c5["k5"] = "v5"
        c5.close()

        self.session.checkpoint()

        if self.is_tiered_scenario():
            self.session.checkpoint('flush_tier=(enabled)')

        # Open a special backup cursor for export operation.
        main_cursor = self.session.open_cursor('backup:export', None, None)
        
        # Copy the file so that we have more information if WT-9203 ever happens again.
        shutil.copyfile('WiredTiger.export', 'WiredTiger.export.original')
        
        # Copy the main database to another directory, including the WiredTiger.export file.
        os.mkdir(self.dir)
        copy_wiredtiger_home(self, '.', self.dir)

        main_cursor.close()
        self.close_conn()

        # Open a connection and session on the directory copy.
        self.conn = self.setUpConnectionOpen(self.dir)
        self.session = self.setUpSessionOpen(self.conn)

        # Create a third table and drop the second table.
        self.session.create(uri_c)
        c6 = self.session.open_cursor(uri_c)
        c6["k6"] = "k6"
        c6.close()

        self.session.checkpoint()

        if self.is_tiered_scenario():
            self.session.checkpoint('flush_tier=(enabled,force=true)')

        self.session.drop(uri_b)

        # Open an export cursor on the database copy.
        wt_export_path = os.path.join(self.dir, "WiredTiger.export")
        export_cursor = self.session.open_cursor('backup:export', None, None)

        # Copy the file so that we have more information if WT-9203 ever happens again.
        shutil.copyfile(wt_export_path, os.path.join(self.dir, "WiredTiger.export.backup"))

        self.assertTrue(os.path.isfile(wt_export_path))
        
        # The information for the third table should exist in the WiredTiger.export file
        # but the information for the second table should not exist in the file.
        with open(wt_export_path, "r") as export_file:
            export_file_string = export_file.read()
            self.assertFalse("exportb" in export_file_string)
            self.assertTrue("exportc" in export_file_string)

        export_cursor.close()

if __name__ == '__main__':
    wttest.run()
