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

import json, os, compatibility_test, wiredtiger


class test_tiered_deprecate(compatibility_test.CompatibilityTestCase):
    '''
    Cross-version coverage for leftover tiered storage after removal.

    Plain upgrade: older writes a normal table (legacy file_meta keys, no
    enabled storage source). Develop opens with config_base=false and reads.

    Plain downgrade: develop creates without those keys. Older fills file_meta
    defaults, reads, alters, checkpoints, and reopens.

    Enabled leftover: older dir_store + flush_tier. Develop with a default
    open (reads WiredTiger.basecfg) returns ENOTSUP. Develop with
    config_base=false opens; the leftover table URI is unsupported.
    Older then reopens the same home with dir_store and reads the rows.
    '''

    older = ['mongodb-8.0', 'mongodb-9.0']
    newer = 'develop'

    build_config = {'standalone': 'true'}
    create_config = 'key_format=i,value_format=S'
    uri = 'table:test_tiered_deprecate'
    file_uri = 'file:test_tiered_deprecate.wt'
    meta_file_uri = 'file:WiredTiger.wt'
    bucket = 'bucket1'
    bucket_prefix = 'pfx_'
    leftover_prefixes = ('object:', 'tier:', 'tiered:')
    leftover_snapshot = 'leftover_meta.json'
    nrows = 100

    # Ignore leftover WiredTiger.basecfg; logging off so log version is not in play.
    open_config = 'create,config_base=false,log=(enabled=false)'

    def test_plain_upgrade(self):
        self.run_method_on_branch(self.older_branch, 'on_older_plain')
        self.run_method_on_branch(self.newer_branch, 'on_newer_plain_upgrade')

    def test_plain_downgrade(self):
        self.run_method_on_branch(self.newer_branch, 'on_newer_plain_create')
        self.run_method_on_branch(self.older_branch, 'on_older_plain_downgrade')

    def test_enabled_leftover(self):
        self.run_method_on_branch(self.older_branch, 'on_older_enabled')
        self.run_method_on_branch(self.newer_branch, 'on_newer_enabled_basecfg')
        self.run_method_on_branch(self.newer_branch, 'on_newer_enabled_no_basecfg')
        self.run_method_on_branch(self.older_branch, 'on_older_enabled_reopen')

    def _dir_store_path(self):
        # The extension exists only in the older build.
        return os.path.join(self.branch_build_path(self.older_branch.name),
          'ext', 'storage_sources', 'dir_store', 'libwiredtiger_dir_store.so')

    def _enabled_conn_config(self):
        ext = self._dir_store_path()
        assert os.path.exists(ext), f'dir_store extension not found: {ext}'
        # Quote the path: standalone builds live in a directory with '=' in the name.
        return (
          'create,tiered_storage=(name=dir_store,bucket=%s,bucket_prefix=%s),'
          'extensions=("%s")' % (self.bucket, self.bucket_prefix, ext))

    def _leftover_meta(self, session):
        meta = session.open_cursor('metadata:')
        leftover = {}
        for k, v in meta:
            assert isinstance(v, str) and v, k
            if k.startswith(self.leftover_prefixes):
                leftover[k] = v
        meta.close()
        return leftover

    def _write_leftover_snapshot(self, leftover):
        with open(self.leftover_snapshot, 'w') as f:
            json.dump(leftover, f, sort_keys=True)

    def _assert_leftover_unchanged(self, session):
        with open(self.leftover_snapshot) as f:
            expected = json.load(f)
        got = self._leftover_meta(session)
        assert got == expected, 'tiered metadata changed:\nexpected %s\ngot %s' % (
          expected, got)

    def _write_rows(self, session):
        c = session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            c[i] = str(i)
        c.close()

    def _check_rows(self, session):
        c = session.open_cursor(self.uri)
        for i in range(1, self.nrows + 1):
            assert c[i] == str(i)
        c.close()

    def _file_meta(self, session, uri):
        meta = session.open_cursor('metadata:')
        value = meta[uri]
        meta.close()
        return value

    def _assert_keys_absent(self, session, uri):
        value = self._file_meta(session, uri)
        assert 'tiered_storage=' not in value, value
        assert 'tiered_object=' not in value, value

    def _assert_keys_present(self, session, uri):
        value = self._file_meta(session, uri)
        assert 'tiered_storage=' in value, value
        assert 'tiered_object=' in value, value

    def on_older_plain(self):
        conn = wiredtiger.wiredtiger_open('.', self.open_config)
        session = conn.open_session()
        session.create(self.uri, self.create_config)
        self._write_rows(session)
        session.checkpoint()
        self._assert_keys_present(session, self.file_uri)
        session.close()
        conn.close()

    def on_newer_plain_upgrade(self):
        conn = wiredtiger.wiredtiger_open('.', self.open_config)
        session = conn.open_session()
        self._check_rows(session)
        self._assert_keys_present(session, self.file_uri)
        session.close()
        conn.close()

    def on_newer_plain_create(self):
        conn = wiredtiger.wiredtiger_open('.', self.open_config)
        session = conn.open_session()
        session.create(self.uri, self.create_config)
        self._write_rows(session)
        session.checkpoint()
        self._assert_keys_absent(session, self.file_uri)
        self._assert_keys_absent(session, self.meta_file_uri)
        session.close()
        conn.close()

    def on_older_plain_downgrade(self):
        conn = wiredtiger.wiredtiger_open('.', self.open_config)
        session = conn.open_session()
        self._check_rows(session)
        session.alter(self.uri, 'access_pattern_hint=random')
        session.checkpoint()
        session.close()
        conn.close()

        conn = wiredtiger.wiredtiger_open('.', self.open_config)
        session = conn.open_session()
        self._check_rows(session)
        session.close()
        conn.close()

    def on_older_enabled(self):
        os.mkdir(self.bucket)
        conn = wiredtiger.wiredtiger_open('.', self._enabled_conn_config())
        session = conn.open_session()
        session.create(self.uri, self.create_config)
        self._write_rows(session)
        session.checkpoint('flush_tier=(enabled)')
        leftover = self._leftover_meta(session)
        assert any(k.startswith('tiered:') for k in leftover), (
          'older branch did not create a tiered: metadata entry')
        self._write_leftover_snapshot(leftover)
        session.close()
        conn.close()

    def on_newer_enabled_basecfg(self):
        # Default open merges WiredTiger.basecfg, which still names dir_store.
        try:
            wiredtiger.wiredtiger_open('.', 'log=(enabled=false)')
            assert False, 'wiredtiger_open of a leftover enabled-tiered home should fail'
        except wiredtiger.WiredTigerError:
            pass
        self.assert_captured_output_contains('stderr', 'tiered storage is not supported')

    def on_newer_enabled_no_basecfg(self):
        conn = wiredtiger.wiredtiger_open('.', self.open_config)
        session = conn.open_session()
        self._assert_leftover_unchanged(session)
        try:
            session.open_cursor(self.uri)
            assert False, 'opening a leftover tiered table should fail'
        except wiredtiger.WiredTigerError:
            pass
        self.assert_captured_output_contains('stderr', 'unsupported object operation')
        session.close()
        conn.close()

    def on_older_enabled_reopen(self):
        conn = wiredtiger.wiredtiger_open('.', self._enabled_conn_config())
        session = conn.open_session()
        self._assert_leftover_unchanged(session)
        self._check_rows(session)
        session.close()
        conn.close()


if __name__ == '__main__':
    compatibility_test.run()
