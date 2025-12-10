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

import compatibility_test, compatibility_version, wiredtiger, errno


class test_flcs_deprecate(compatibility_test.CompatibilityTestCase):
    '''
    Test FLCS deprecation handling during database upgrade.
    '''

    build_config = {'standalone': 'true'}
    conn_config = ''
    create_config = 'key_format=r,value_format=8t'
    uri = 'table:test_flcs_deprecate'
    nrows = 100

    def test_flcs_deprecate(self):

        flcs_deprecated_version = compatibility_version.WTVersion("mongodb-8.3")

        # Test FLCS table creation fails on FLCS deprecated version
        if self.older_branch >= flcs_deprecated_version:
            self.run_method_on_branch(self.newer_branch, 'flcs_table_creation_unsupported')
            return

        # Only run this test for older branch where FLCS is still available and newer branch where FLCS is deprecated.
        if self.older_branch < flcs_deprecated_version and self.newer_branch >= flcs_deprecated_version:
            # Run the older-branch part (create the FLCS table and populate it)
            self.run_method_on_branch(self.older_branch, 'on_older_branch')
            # Run the newer-branch part (attempt to open and expect failure)
            self.run_method_on_branch(self.newer_branch, 'on_newer_branch')

    def flcs_table_creation_unsupported(self):
        # Test that FLCS table creation is unsupported on branches later than the deprecation version.
        try:
            conn = wiredtiger.wiredtiger_open('.', 'create,' + self.conn_config)
            session = conn.open_session()
            session.create(self.uri, self.create_config)
            assert False
        except wiredtiger.WiredTigerError as e:
            assert str(e) in wiredtiger.wiredtiger_strerror(errno.ENOTSUP)

    def on_older_branch(self):
        # This runs in the older branch to create a FLCS table and populate it.
        conn = wiredtiger.wiredtiger_open('.', 'create,' + self.conn_config)
        session = conn.open_session()

        session.create(self.uri, self.create_config)

        c = session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            c[i] = i
        c.close()
        session.close()
        conn.close()

    def on_newer_branch(self):
        # Expect opening the FLCS table to fail because FLCS is deprecated.
        try:
            wiredtiger.wiredtiger_open('.', self.conn_config)
            assert False
        except wiredtiger.WiredTigerError as e:
            assert str(e) in wiredtiger.wiredtiger_strerror(wiredtiger.WT_PANIC)

