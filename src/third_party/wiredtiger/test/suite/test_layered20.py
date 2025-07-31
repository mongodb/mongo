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

import platform, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered20.py
# Test 32 consective deltas

@disagg_test_class
class test_layered20(wttest.WiredTigerTestCase, DisaggConfigMixin):
    encrypt = [
        ('none', dict(encryptor='none', encrypt_args='')),
        ('rotn', dict(encryptor='rotn', encrypt_args='keyid=13')),
    ]

    compress = [
        ('none', dict(block_compress='none')),
        ('snappy', dict(block_compress='snappy')),
    ]

    uris = [
        ('layered', dict(uri='layered:test_layered20')),
        ('btree', dict(uri='file:test_layered20')),
    ]

    ts = [
        ('ts', dict(ts=True)),
        ('non-ts', dict(ts=False)),
    ]

    conn_base_config = 'transaction_sync=(enabled,method=fsync),statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'page_delta=(delta_pct=100),disaggregated=(page_log=palm),'
    disagg_storages = gen_disagg_storages('test_layered20', disagg_only = True)

    # Make scenarios for different cloud service providers
    scenarios = make_scenarios(encrypt, compress, disagg_storages, uris, ts)

    nitems = 10

    def session_create_config(self):
        # The delta percentage of 200 is an arbitrary large value, intended to produce
        # deltas a lot of the time.
        cfg = 'key_format=S,value_format=S,block_compressor={}'.format(self.block_compress)
        if self.uri.startswith('file'):
            cfg += ',block_manager=disagg'
        return cfg

    def conn_config(self):
        enc_conf = 'encryption=(name={0},{1})'.format(self.encryptor, self.encrypt_args)
        return self.conn_base_config + 'disaggregated=(role="leader"),' + enc_conf

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        extlist.extension('compressors', self.block_compress)
        extlist.extension('encryptors', self.encryptor)
        DisaggConfigMixin.conn_extensions(self, extlist)

    def test_layered_read_write(self):
        if platform.processor() == 's390x':
            self.skipTest("FIXME-WT-15000: not working on zSeries")

        self.pr('CREATING')
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "a" * 100

        for i in range(self.nitems):
            self.session.begin_transaction()
            cursor[str(i)] = value1
            if self.ts:
                self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(5))
            else:
                self.session.commit_transaction()

        self.session.checkpoint()

        for j in range(32):
            for i in range(self.nitems):
                if i % 10 == 0:
                    self.session.begin_transaction()
                    cursor[str(i)] = str(10 + 5 * j)
                    if self.ts:
                        self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(10 + 5 * j))
                    else:
                        self.session.commit_transaction()

            self.session.checkpoint()

        follower_config = self.conn_base_config + 'disaggregated=(role="follower",' +\
            f'checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")'
        self.reopen_conn(config = follower_config)

        cursor = self.session.open_cursor(self.uri, None, None)

        if self.ts:
            self.session.begin_transaction("read_timestamp=" + self.timestamp_str(5))
            for i in range(self.nitems):
                self.assertEqual(cursor[str(i)], value1)
            self.session.rollback_transaction()

            for j in range(32):
                self.session.begin_transaction("read_timestamp=" + self.timestamp_str(10 + 5 * j))
                for i in range(self.nitems):
                    if i % 10 == 0:
                        self.assertEqual(cursor[str(i)], str(10 + 5 * j))
                    else:
                        self.assertEqual(cursor[str(i)], value1)
                self.session.rollback_transaction()
        else:
            for i in range(self.nitems):
                if i % 10 == 0:
                    self.assertEqual(cursor[str(i)], str(10 + 5 * 31))
                else:
                    self.assertEqual(cursor[str(i)], value1)
