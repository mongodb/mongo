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
import os, re
from wtbackup import backup_base
from wtscenario import make_scenarios

# test_backup28.py
# Test selective backup with different schema types. Recovering a partial backup with target uris
# including colgroups, index or lsm formats should raise a message. The only supported types are
# table formats in the uri list.
class test_backup28(backup_base):
    dir='backup.dir'    # Backup directory name
    uri="table_backup"

    types = [
        ('file', dict(pfx='file:', target_uri_list=["file:table0"])),
        ('lsm', dict(pfx='lsm:', target_uri_list=["lsm:table0"])),
        ('table-simple', dict(pfx='table:', target_uri_list=["table:table0"])),
        ('table-cg', dict(pfx='table:', target_uri_list=["index:table0:i0", "table:table0"])),
        ('table-index', dict(pfx='table:', target_uri_list=["colgroup:table0:g0", "table:table0"])),
    ]

    scenarios = make_scenarios(types)

    def test_backup28(self):
        selective_remove_file_list = []
        uri = self.pfx + 'table0'
        create_params = 'key_format=S,value_format=S,'
    
        cgparam = 'columns=(k,v),colgroups=(g0),'
        # Create the main table.
        self.session.create(uri, create_params + cgparam)

        if (self.pfx != "lsm:" and self.pfx != "file:"):
            # Add in column group and index tables.
            colgroup_param = 'columns=(v),'
            suburi = 'colgroup:table0:g0'
            self.session.create(suburi, colgroup_param)
            
            suburi = 'index:table0:i0'
            self.session.create(suburi, cgparam)
            self.session.checkpoint()

        os.mkdir(self.dir)

        # Now copy the files using full backup. Selectively don't copy files based on remove list.
        all_files = self.take_selective_backup(self.dir, [])
        
        target_uri_list_format = str(self.target_uri_list).replace("\'", "\"")
        if len(self.target_uri_list) and self.target_uri_list[0] == "table:table0":
            # After the full backup, open and recover the backup database, and it should succeed.
            backup_conn = self.wiredtiger_open(self.dir, "backup_restore_target={0}".format(target_uri_list_format))
            bkup_session = backup_conn.open_session()
            
            # Make sure that the table recovered properly.
            c = bkup_session.open_cursor(uri, None, None)
            c.close()
            backup_conn.close()
        else:
            # After the full backup, perform partial backup restore adding the target uris of 
            # indexes, colgroups or lsm. This should fail and return with a message, as we only allow 
            # table formats.
            self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
                lambda: self.wiredtiger_open(self.dir, "backup_restore_target={0}".format(target_uri_list_format)),
                '/partial backup restore only supports objects of type .* formats in the target uri list/')

            
if __name__ == '__main__':
    wttest.run()
