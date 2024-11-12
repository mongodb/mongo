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
from wiredtiger import stat

# test_bug035.py
# This test validates a fix for a bug (WT-13716) related to selective backup and fast truncate.
# The bug allowed fast-truncated history store pages to reappear in the backup after
# a shutdown. This test verifies that if a selective backup is taken, only the data from selected
# tables (as part of backup) are retained in the history store and metadata, and the unwanted
# tables are removed from HS and metadata.
class test_bug035(backup_base):
    conn_config = 'cache_size=1G'
    dir='backup.dir'
    uris = [f"table:uri_{i}" for i in range(1, 11)]

    def add_timestamp_data(self, uri, key, val, timestamp):
        self.session.begin_transaction()
        c = self.session.open_cursor(uri, None, None)
        for i in range(0, 1000):
            k = key + str(i)
            v = val
            c[k] = v
        c.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(timestamp))

    def test_bug035(self):
        for uri in self.uris:
            # create 10 URIs and add large amount of data in each.
            self.session.create(uri, "key_format=S,value_format=S")
            for i in range(1, 10):
                self.add_timestamp_data(uri, "key", f"val{i}", i)

        # Ensure all the data added is stable to persist data in HS.
        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(15))
        self.session.checkpoint()

        # Create a backup directory.
        os.mkdir(self.dir)

        # Take a selective backup of the first 5 tables by specifying the last 5 tables to be
        # removed in `take_selective_backup`.
        last_5_tables = [uri.replace("table:", "") + ".wt" for uri in self.uris[-5:]]
        self.take_selective_backup(self.dir, last_5_tables)

        # Open the backup directory. As part of opening, it will run RTS internally to truncate any HS pages
        # that belong to the tables that are not part of the selective backup (i.e. `last_5_tables`).
        first_5_tables = '","'.join(self.uris[:5])
        backup_conn = self.wiredtiger_open(self.dir, "backup_restore_target=[\"{0}\"]".format(first_5_tables))
        backup_session = backup_conn.open_session()
        stat_cursor = backup_session.open_cursor('statistics:', None, None)

        # Assert that fast truncate was performed.
        fast_truncate_pages = stat_cursor[stat.conn.rec_page_delete_fast][2]
        self.assertGreater(fast_truncate_pages, 0)

        # Reopen the connection with verify_metadata=true to ensure the excluded tables (`last_5_tables`)
        # are absent in HS and metadata.
        backup_conn.close()
        backup_conn = self.wiredtiger_open(self.dir, "verify_metadata=true")
        backup_conn.close()
