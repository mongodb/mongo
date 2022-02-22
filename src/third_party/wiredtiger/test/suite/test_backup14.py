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

import os
from wtbackup import backup_base

# test_backup14.py
# Test cursor backup with a block-based incremental cursor.
class test_backup14(backup_base):
    conn_config='cache_size=1G,log=(enabled,file_max=100K)'
    dir='backup.dir'                    # Backup directory name
    logmax="100K"
    uri="table:main"
    uri2="table:extra"
    uri_logged="table:logged_table"
    uri_not_logged="table:not_logged_table"

    bkp_home = "WT_BLOCK"
    home_full = "WT_BLOCK_LOG_FULL"
    home_incr = "WT_BLOCK_LOG_INCR"
    logpath = "logpath"
    nops = 1000
    max_iteration=7
    new_table=False

    pfx = 'test_backup'
    # Set the key and value big enough that we modify a few blocks.
    bigkey = 'Key' * 100
    bigval = 'Value' * 100

    #
    # Remove data from uri (table:main).
    #
    def remove_data(self):
        c = self.session.open_cursor(self.uri)
        #
        # We run the outer loop until mult value to make sure we remove all the inserted records
        # from the main table.
        #
        for i in range(0, self.mult):
            for j in range(i, self.nops):
                num = j + (i * self.nops)
                key = self.bigkey + str(num)
                c.set_key(key)
                self.assertEquals(c.remove(), 0)
        c.close()
        # Increase the counter so that later backups have unique ids.
        self.bkup_id += 1

    #
    # This function will add records to the table (table:main), take incremental/full backups and
    # validate the backups.
    #
    def add_data_validate_backups(self):
        self.pr('Adding initial data')
        self.initial_backup = True
        self.add_data(self.uri, self.bigkey, self.bigval, True)

        self.take_full_backup(self.home_incr)
        self.initial_backup = False

        self.add_data(self.uri, self.bigkey, self.bigval, True)

        self.take_full_backup(self.home_full)
        self.take_incr_backup(self.home_incr)
        self.compare_backups(self.uri, self.home_full, self.home_incr, str(self.bkup_id))
        self.setup_directories(self.home_incr, self.home_full)

    #
    # This function will remove all the records from table (table:main), take backup and validate the
    # backup.
    #
    def remove_all_records_validate(self):
        self.remove_data()
        self.session.checkpoint()
        self.take_full_backup(self.home_full)
        self.take_incr_backup(self.home_incr)
        self.compare_backups(self.uri, self.home_full, self.home_incr, str(self.bkup_id))
        self.setup_directories(self.home_incr, self.home_full)

    #
    # This function will drop the existing table uri (table:main) that is part of the backups and
    # create new table uri2 (table:extra), take incremental backup and validate.
    #
    def drop_old_add_new_table(self):

        # Drop main table.
        self.session.drop(self.uri)

        # Create uri2 (table:extra).
        self.session.create(self.uri2, "key_format=S,value_format=S")

        self.new_table = True
        self.add_data(self.uri2, self.bigkey, self.bigval, True)
        self.take_incr_backup(self.home_incr)

        table_list = 'tablelist.txt'
        # Assert if the dropped table (table:main) exists in the incremental folder.
        self.runWt(['-R', '-h', self.home, 'list'], outfilename=table_list)
        ret = os.system("grep " + self.uri + " " + table_list)
        self.assertNotEqual(ret, 0, self.uri + " dropped, but table exists in " + self.home)
        self.setup_directories(self.home_incr, self.home_full)

    #
    # This function will create previously dropped table uri (table:main) and add different content to
    # it, take backups and validate the backups.
    #
    def create_dropped_table_add_new_content(self):
        self.session.create(self.uri, "key_format=S,value_format=S")
        self.add_data(self.uri, self.bigkey, self.bigval, True)
        self.take_full_backup(self.home_full)
        self.take_incr_backup(self.home_incr)
        self.compare_backups(self.uri, self.home_full, self.home_incr, str(self.bkup_id))
        self.setup_directories(self.home_incr, self.home_full)

    #
    # This function will insert bulk data in logged and not-logged table, take backups and validate the
    # backups.
    #
    def insert_bulk_data(self):
        #
        # Insert bulk data into uri3 (table:logged_table).
        #
        self.session.create(self.uri_logged, "key_format=S,value_format=S")
        self.add_data(self.uri_logged, self.bigkey, self.bigval, True)

        self.take_full_backup(self.home_full)
        self.take_incr_backup(self.home_incr)
        self.compare_backups(self.uri_logged, self.home_full, self.home_incr, str(self.bkup_id))
        self.setup_directories(self.home_incr, self.home_full)
        #
        # Insert bulk data into uri4 (table:not_logged_table).
        #
        self.session.create(self.uri_not_logged, "key_format=S,value_format=S,log=(enabled=false)")
        self.add_data(self.uri_not_logged, self.bigkey, self.bigval, True)

        self.take_full_backup(self.home_full)
        self.take_incr_backup(self.home_incr)
        self.compare_backups(self.uri_not_logged, self.home_full, self.home_incr, str(self.bkup_id))
        self.setup_directories(self.home_incr, self.home_full)

    def test_backup14(self):
        os.mkdir(self.bkp_home)
        self.home = self.bkp_home
        self.session.create(self.uri, "key_format=S,value_format=S")

        self.setup_directories(self.home_incr, self.home_full)

        self.pr('*** Add data, checkpoint, take backups and validate ***')
        self.add_data_validate_backups()

        self.pr('*** Remove old records and validate ***')
        self.remove_all_records_validate()

        self.pr('*** Drop old and add new table ***')
        self.drop_old_add_new_table()

        self.pr('*** Create previously dropped table and add new content ***')
        self.create_dropped_table_add_new_content()

        self.pr('*** Insert data into Logged and Not-Logged tables ***')
        self.cursor_config = 'bulk'
        self.insert_bulk_data()

if __name__ == '__main__':
    wttest.run()
