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

import compatibility_test, compatibility_version, errno, wiredtiger


class test_chunkcache_deprecate(compatibility_test.CompatibilityTestCase):
    '''
    Test chunk cache deprecation handling across a database upgrade.

    A database created on a branch that supports chunk cache may persist chunk_cache settings in
    its WiredTiger.basecfg file. When the database is later opened on a branch where chunk cache
    has been removed:
      - If chunk_cache.enabled=false in the basecfg, the open should succeed with a deprecation
        warning and the configuration should be ignored.
      - If chunk_cache.enabled=true in the basecfg, the open should fail with a clean deprecation
        error.
    '''

    build_config = {'standalone': 'true'}
    conn_config = ''
    create_config = 'key_format=i,value_format=S'
    uri = 'table:test_chunkcache_deprecate'
    nrows = 100

    # Config used on the older branch to persist chunk_cache settings in WiredTiger.basecfg.
    # The non-default capacity ensures the chunk_cache subgroup is written to the basecfg, and
    # enabled=false avoids needing tiered storage to be configured.
    older_create_config = 'chunk_cache=(enabled=false,capacity=1GB)'

    def test_chunkcache_deprecate(self):

        # Chunk cache was introduced in mongodb-8.0: earlier branches reject the config key.
        cc_introduced_version = compatibility_version.WTVersion("mongodb-8.0")
        # Chunk cache deprecation currently lives only on develop (WT-17169). Update this
        # boundary if/when the change is back ported to a release branch.
        cc_deprecated_version = compatibility_version.WTVersion("develop")

        # If the older branch is already at or past the deprecation version, just verify that
        # opening with chunk_cache enabled fails on the newer branch.
        if self.older_branch >= cc_deprecated_version:
            self.run_method_on_branch(self.newer_branch, 'chunk_cache_enabled_unsupported')
            return

        # Skip branches before chunk cache implementation.
        if self.older_branch < cc_introduced_version:
            return

        if self.older_branch < cc_deprecated_version and self.newer_branch >= cc_deprecated_version:
            self.run_method_on_branch(self.older_branch, 'on_older_branch')

            self.run_method_on_branch(self.newer_branch, 'on_newer_branch_disabled')

            # Flip the basecfg to enabled=true and verify the newer branch rejects it cleanly.
            self._set_basecfg_chunkcache_enabled(True)
            self.run_method_on_branch(self.newer_branch, 'on_newer_branch_enabled')

    def _set_basecfg_chunkcache_enabled(self, enabled):
        basecfg_path = 'WiredTiger.basecfg'
        with open(basecfg_path, 'r') as f:
            contents = f.read()
        old_val = 'enabled=false' if enabled else 'enabled=true'
        new_val = 'enabled=true' if enabled else 'enabled=false'
        assert old_val in contents, f'{old_val} not found in {basecfg_path}'
        with open(basecfg_path, 'w') as f:
            f.write(contents.replace(old_val, new_val))

    def chunk_cache_enabled_unsupported(self):
        # Runs on a branch where chunk cache has been deprecated: opening with
        # chunk_cache.enabled=true should fail with a clean ENOTSUP deprecation error.
        try:
            wiredtiger.wiredtiger_open('.', 'create,chunk_cache=(enabled=true)')
            assert False, 'wiredtiger_open with chunk_cache=(enabled=true) should fail'
        except wiredtiger.WiredTigerError as e:
            assert str(e) == wiredtiger.wiredtiger_strerror(errno.ENOTSUP)
        self.assert_captured_output_contains('stderr', 'chunk cache has been deprecated')

    def on_older_branch(self):
        # Runs on the older branch to create a database and persist chunk_cache configuration in
        # WiredTiger.basecfg.
        conn = wiredtiger.wiredtiger_open('.', 'create,' + self.older_create_config)
        session = conn.open_session()

        session.create(self.uri, self.create_config)

        c = session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            c[i] = str(i)
        c.close()
        session.close()
        conn.close()

    def on_newer_branch_disabled(self):
        # Runs on the newer branch: opening should succeed with a deprecation warning because
        # chunk_cache is configured but disabled in the basecfg.
        conn = wiredtiger.wiredtiger_open('.', self.conn_config)

        # Assert the expected deprecation warning was emitted during wiredtiger_open.
        self.assert_captured_output_contains('stdout', 'chunk cache has been deprecated')

        session = conn.open_session()

        # Verify the data written on the older branch is readable and intact, i.e. the upgraded
        # database is actually usable  not just that wiredtiger_open returned successfully.
        c = session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            assert c[i] == str(i), f'data mismatch on key {i}'
        c.close()
        session.close()
        conn.close()

    def on_newer_branch_enabled(self):
        # Runs on the newer branch with basecfg chunk_cache.enabled=true:
        # opening should fail with a clean ENOTSUP deprecation error.
        try:
            wiredtiger.wiredtiger_open('.', self.conn_config)
            assert False, 'wiredtiger_open should fail when basecfg has chunk_cache enabled'
        except wiredtiger.WiredTigerError as e:
            assert str(e) == wiredtiger.wiredtiger_strerror(errno.ENOTSUP)
        self.assert_captured_output_contains('stderr', 'chunk cache has been deprecated')


if __name__ == '__main__':
    compatibility_test.run()
