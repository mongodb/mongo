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

import wiredtiger
from wtscenario import make_scenarios
from wtbackup import backup_base

# test_import10.py
#    Run import/export while backup cursor is open.
class test_import10(backup_base):
    create_config = 'allocation_size=512,key_format=i,value_format=i'
    dir='backup.dir'                    # Backup directory name
    uri = 'test_import10'

    scenarios = make_scenarios([
        ('import_with_metadata', dict(repair=False)),
        ('import_repair', dict(repair=True)),
    ])

    def test_import_with_open_backup_cursor(self):
        # Create and populate the table.
        table_uri = 'table:' + self.uri
        self.session.create(table_uri, self.create_config)
        cursor = self.session.open_cursor(table_uri)
        for i in range(1, 1000):
            cursor[i] = i
        cursor.close()
        self.session.checkpoint()

        # Export the metadata for the file.
        file_uri = 'file:' + self.uri + '.wt'
        c = self.session.open_cursor('metadata:', None, None)
        original_db_table_config = c[table_uri]
        original_db_file_config = c[file_uri]
        c.close()

        self.session.drop(table_uri, 'remove_files=false')

        # Verify the table no longer exists in the database by attempting to open a cursor.
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(table_uri))

        # Open backup cursor.
        bkup_c = self.session.open_cursor('backup:', None, None)

        # First construct the config string for the default or repair import scenario,
        # then call create to import the table.
        if self.repair:
            import_config = 'import=(enabled,repair=true)'
        else:
            import_config = '{},import=(enabled,repair=false,file_metadata=({}))'.format(
                original_db_table_config, original_db_file_config)
        self.session.create(table_uri, import_config)

        # Verify object.
        self.verifyUntilSuccess(self.session, table_uri)

        # Check that the data got imported correctly.
        cursor = self.session.open_cursor(table_uri)
        for i in range(1, 1000):
            self.assertEqual(cursor[i], i)
        cursor.close()

        all_files = self.take_full_backup(self.dir, bkup_c)
        self.assertTrue(self.uri + ".wt" not in all_files)
        bkup_c.close()

if __name__ == '__main__':
    wttest.run()
