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
from wtbackup import backup_base

# Test which identifiers an incremental backup requires.
#
# A cursor opened with only a source identifier is a valid incremental cursor and needs no
# this_id. Opening and closing it without iterating, which is how test/format checks that a
# source identifier is still valid, creates no identifier; the check can be repeated and used
# across a reopen.
#
# A this_id is required when the backup is taken rather than merely checked: it names the new
# identifier that a subsequent incremental backup uses as its source.
class test_backup32(backup_base):
    test_name = __qualname__
    uri = f'table:{test_name}'

    def setUp(self):
        super(test_backup32, self).setUp()
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.add_data(self.uri, 'key', 'value', True)

    # The identifiers known to incremental backup.
    def registered_ids(self):
        ids = []
        bkup_c = self.session.open_cursor('backup:query_id', None, None)
        # We cannot use 'for idstr in bkup_c:' usage because backup cursors don't have
        # values and adding in get_values returns ENOTSUP and causes the usage to fail.
        while True:
            ret = bkup_c.next()
            if ret != 0:
                break
            ids.append(bkup_c.get_key())
        bkup_c.close()
        ids.sort()
        return ids

    # Establish a source identifier with a full incremental backup.
    def full_backup(self, this_id):
        bkup_c = self.session.open_cursor(
            'backup:', None, f'incremental=(enabled,granularity=4k,this_id="{this_id}")')
        while bkup_c.next() == 0:
            pass
        bkup_c.close()
        self.session.checkpoint()

    def test_checking_source_needs_no_this_id(self):
        self.full_backup('ID1')
        self.assertEqual(self.registered_ids(), ['ID1'])

        # Check the source the way test/format does: open and close without iterating.
        bkup_c = self.session.open_cursor('backup:', None, 'incremental=(enabled,src_id="ID1")')
        bkup_c.close()

        # No identifier was created, so the check can be repeated and survives a reopen.
        bkup_c = self.session.open_cursor('backup:', None, 'incremental=(enabled,src_id="ID1")')
        bkup_c.close()
        self.reopen_conn()
        bkup_c = self.session.open_cursor('backup:', None, 'incremental=(enabled,src_id="ID1")')
        bkup_c.close()
        self.assertEqual(self.registered_ids(), ['ID1'])

    def test_checking_unknown_source_fails(self):
        self.full_backup('ID1')
        self.assertRaisesException(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor('backup:', None,
                'incremental=(enabled,src_id="NOSUCH")'))

    def test_taking_backup_needs_a_this_id(self):
        self.full_backup('ID1')

        # Taking an incremental backup registers the new identifier, so a later backup can use
        # it as a source. A source-only cursor does not, which is why the this_id is required
        # when the backup is taken rather than only checked.
        bkup_c = self.session.open_cursor(
            'backup:', None, 'incremental=(enabled,src_id="ID1",this_id="ID2")')
        while bkup_c.next() == 0:
            pass
        bkup_c.close()
        self.assertEqual(self.registered_ids(), ['ID1', 'ID2'])

        # And the new identifier is now usable as a source.
        bkup_c = self.session.open_cursor('backup:', None, 'incremental=(enabled,src_id="ID2")')
        bkup_c.close()
