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

import re
from wtbackup import backup_base

# test_backup30.py
#    A file whose metadata record carries no incremental backup information must still be
# checkpointed, and must still be backed up correctly, while an incremental backup identifier
# is configured. Metadata written by a release that predates the field looks exactly like
# this, and such a file is only visited again once something dirties it.
class test_backup30(backup_base):
    conn_config = 'statistics=(fast)'
    create_config = 'allocation_size=512,key_format=S,value_format=S'

    uri = 'table:legacy'
    file_uri = 'file:legacy.wt'
    home_full = 'WT_BLOCK_FULL'
    home_incr = 'WT_BLOCK_INCR'

    nops = 2000
    bigkey = 'Key' * 20
    bigval = 'Value' * 20

    def backup_info_present(self):
        cursor = self.session.open_cursor('metadata:', None, None)
        config = cursor[self.file_uri]
        cursor.close()
        return 'checkpoint_backup_info' in config

    def remove_backup_info(self):
        cursor = self.session.open_cursor('metadata:', None, 'readonly=false')
        config = cursor[self.file_uri]
        stripped = re.sub(r',checkpoint_backup_info=\([^)]*\)', '', config)
        stripped = re.sub(r',checkpoint_backup_info=(?=,|$)', '', stripped)
        self.assertNotEqual(stripped, config)
        cursor[self.file_uri] = stripped
        cursor.close()

    def open_backup_cursor(self, this_id):
        config = 'incremental=(enabled,granularity=4k,this_id="' + this_id + '")'
        return self.session.open_cursor('backup:', None, config)

    # Leave the file in the state a pre-4.2.4 metadata record describes: an identifier is
    # configured on the connection, but the file's own record says nothing about backups.
    # The connection is reopened so the in-memory block modification state is rebuilt from
    # the record rather than left over from before it was rewritten.
    def make_record_legacy(self):
        self.assertTrue(self.backup_info_present())
        self.remove_backup_info()
        self.assertFalse(self.backup_info_present())
        self.reopen_conn()
        self.assertFalse(self.backup_info_present())

    # The checkpoint that first visits such a file must complete rather than panic, and must
    # supply the missing field so the file rejoins normal tracking.
    def test_checkpoint_without_backup_info(self):
        self.session.create(self.uri, self.create_config)
        self.add_data(self.uri, self.bigkey, self.bigval, True)

        backup_cursor = self.open_backup_cursor('ID0')
        backup_cursor.close()

        self.make_record_legacy()

        # A clean tree is skipped by checkpoint indefinitely, so the file has to be dirtied
        # for a checkpoint to reach it at all.
        self.add_data(self.uri, self.bigkey, self.bigval, True)
        self.assertTrue(self.backup_info_present())

        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor[self.bigkey + '0'], self.bigval + '0')
        cursor.close()

    # An incremental backup taken across that checkpoint has to reproduce the file exactly.
    # Nothing recorded which blocks changed while the record carried no backup information,
    # so a backup that trusts the healed bitmap alone would silently omit them.
    def test_incremental_backup_without_backup_info(self):
        self.setup_directories(self.home_incr, self.home_full)
        self.session.create(self.uri, self.create_config)
        self.add_data(self.uri, self.bigkey, self.bigval, True)

        # Establish the identifier and seed the incremental directory with a full copy.
        backup_cursor = self.open_backup_cursor('ID0')
        self.take_full_backup(self.home_incr, backup_cursor)
        backup_cursor.close()

        self.make_record_legacy()

        # Dirty the file. This is the checkpoint that panics without the fix, and the one
        # whose changes the incremental backup below has to account for.
        self.add_data(self.uri, self.bigkey, self.bigval, True)
        self.assertTrue(self.backup_info_present())

        self.take_full_backup(self.home_full)
        self.take_incr_backup(self.home_incr, src_id=0, dest_id=1)
        self.compare_backups(self.uri, self.home_full, self.home_incr)
