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

import errno, os, re, wiredtiger, wttest
from wtscenario import make_scenarios

class _tiered_uri_unsupported:
    def _uri(self):
        return self.prefix + 'test_tiered_unsupported'

    def _assert_unsupported(self, expr):
        uri = self._uri()
        msg = f'{self.err_prefix}: {uri}'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError, expr, '/' + re.escape(msg) + '/')
        err, _sub, last_msg = self.session.get_last_error()
        self.assertEqual(err, errno.ENOTSUP)
        self.assertEqual(last_msg, msg)

# Test that removed tiered storage URIs are refused.
class test_tiered_unsupported(_tiered_uri_unsupported, wttest.WiredTigerTestCase):
    uri_types = [
        ('object', dict(prefix='object:', err_prefix='unsupported object operation')),
        ('tier', dict(prefix='tier:', err_prefix='unknown object type')),
        ('tiered', dict(prefix='tiered:', err_prefix='unsupported object operation')),
    ]
    scenarios = make_scenarios(uri_types)

    def test_drop(self):
        self._assert_unsupported(lambda: self.session.drop(self._uri()))

    def test_alter(self):
        self._assert_unsupported(
            lambda: self.session.alter(self._uri(), 'access_pattern_hint=random'))

    def test_verify(self):
        self._assert_unsupported(lambda: self.session.verify(self._uri()))

    def test_salvage(self):
        self._assert_unsupported(lambda: self.session.salvage(self._uri()))

    def test_compact(self):
        self._assert_unsupported(lambda: self.session.compact(self._uri()))

    def test_create(self):
        self._assert_unsupported(lambda: self.session.create(self._uri()))

    def test_open_cursor(self):
        self._assert_unsupported(lambda: self.session.open_cursor(self._uri()))

    def test_statistics(self):
        uri = 'statistics:' + self._uri()
        msg = 'unsupported object operation: ' + uri
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, lambda: self.session.open_cursor(uri),
            '/' + re.escape(msg) + '/')
        err, _sub, last_msg = self.session.get_last_error()
        self.assertEqual(err, errno.ENOTSUP)
        self.assertEqual(last_msg, msg)

class test_tiered_unsupported_api(wttest.WiredTigerTestCase):
    def test_flush_tier(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.checkpoint('flush_tier=(enabled)'),
            '/unknown configuration key/')

    def test_reconfigure_tiered_storage(self):
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure('tiered_storage=(local_retention=300)'),
            '/unknown configuration key/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.conn.reconfigure('tiered_storage=(name=dir_store)'),
            '/unknown configuration key/')

    def test_conn_tiered_storage(self):
        msg = 'tiered storage is not supported'
        os.mkdir('ts_home')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.wiredtiger_open('ts_home', 'create,tiered_storage=(name=dir_store)'),
            '/' + re.escape(msg) + '/')

    def test_create_tiered_storage(self):
        msg = 'tiered storage is not supported'
        self.session.create('table:ts_none', 'tiered_storage=(name=none)')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create('table:ts_name', 'tiered_storage=(name=dir_store)'),
            '/' + re.escape(msg) + '/')
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create('file:ts_name.wt', 'tiered_storage=(name=dir_store)'),
            '/' + re.escape(msg) + '/')

class test_tiered_unsupported_file_meta(wttest.WiredTigerTestCase):
    uri_types = [
        ('table', dict(uri='table:ts_meta', file_uri='file:ts_meta.wt')),
        ('file', dict(uri='file:ts_meta.wt', file_uri='file:ts_meta.wt')),
    ]
    scenarios = make_scenarios(uri_types)

    leftover = (
        'tiered_object=false,'
        'tiered_storage=(auth_token=,bucket=,bucket_prefix=,'
        'cache_directory=,local_retention=300,name=none,'
        'object_target_size=0,shared=false)')

    def setUp(self):
        if self.runningHook('disagg') and self.uri.startswith('table:'):
            self.skipTest(
                'Disagg rewrites table: creates; this test assumes ordinary file metadata')
        super().setUp()

    def _file_metadata(self):
        md = self.session.open_cursor('metadata:')
        value = md[self.file_uri]
        md.close()
        return value

    def _assert_keys_absent(self):
        value = self._file_metadata()
        self.assertNotIn('tiered_storage=', value)
        self.assertNotIn('tiered_object=', value)

    def _inject_leftover_keys(self):
        md = self.session.open_cursor('metadata:', None, 'readonly=0')
        md.set_key(self.file_uri)
        self.assertEqual(md.search(), 0)
        md.set_value(md.get_value() + ',' + self.leftover)
        self.assertEqual(md.update(), 0)
        md.close()

    def test_create_does_not_persist(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self._assert_keys_absent()
        self.session.checkpoint()
        self._assert_keys_absent()
        self.reopen_conn()
        self._assert_keys_absent()
        self.session.alter(self.uri, 'access_pattern_hint=random')
        self._assert_keys_absent()

    def test_create_name_none_does_not_persist(self):
        self.session.create(
            self.uri, 'key_format=S,value_format=S,tiered_storage=(name=none)')
        self._assert_keys_absent()

    def test_leftover_keys_still_open(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.reopen_conn()
        self._inject_leftover_keys()
        self.reopen_conn()
        c = self.session.open_cursor(self.uri)
        c['a'] = 'b'
        self.assertEqual(c['a'], 'b')
        c.close()
        value = self._file_metadata()
        self.assertIn('tiered_storage=', value)
        self.assertIn('tiered_object=', value)
        self.session.alter(self.uri, 'access_pattern_hint=random')
        self._assert_keys_absent()

class test_tiered_unsupported_truncate(_tiered_uri_unsupported, wttest.WiredTigerTestCase):
    uri_types = [
        ('object', dict(prefix='object:', err_prefix='unsupported object operation')),
        ('tier', dict(prefix='tier:', err_prefix='unknown object type')),
        ('tiered', dict(prefix='tiered:', err_prefix='unsupported object operation')),
    ]
    scenarios = make_scenarios(uri_types)

    def test_truncate(self):
        self._assert_unsupported(lambda: self.session.truncate(self._uri(), None, None, None))
