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

import wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered08.py
# Simple read write testing using the page log API

@disagg_test_class
class test_layered08(wttest.WiredTigerTestCase, DisaggConfigMixin):
    encrypt = [
        ('none', dict(encryptor='none', encrypt_args='')),
        ('rotn', dict(encryptor='rotn', encrypt_args='keyid=13')),
    ]

    compress = [
        ('none', dict(block_compress='none')),
        ('snappy', dict(block_compress='snappy')),
    ]

    conn_base_config = 'transaction_sync=(enabled,method=fsync),statistics=(all),statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'disaggregated=(page_log=palm),'
    disagg_storages = gen_disagg_storages('test_layered08', disagg_only = True)

    scenarios = make_scenarios(encrypt, compress, disagg_storages)

    nitems = 10000

    def conn_config(self):
        enc_conf = 'encryption=(name={0},{1})'.format(self.encryptor, self.encrypt_args)
        return self.conn_base_config + 'disaggregated=(role="leader"),' + enc_conf

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        extlist.extension('compressors', self.block_compress)
        extlist.extension('encryptors', self.encryptor)
        DisaggConfigMixin.conn_extensions(self, extlist)

    def test_layered_read_write(self):
        uri = "layered:test_layered08"
        create_session_config = 'key_format=S,value_format=S,block_compressor={}'.format(self.block_compress)
        self.pr('CREATING')
        self.session.create(uri, create_session_config)

        cursor = self.session.open_cursor(uri, None, None)

        for i in range(self.nitems):
            cursor["Hello " + str(i)] = "World"

        self.session.checkpoint()

        # XXX
        # Inserted timing delays around reopen, apparently needed because of the
        # layered table watcher implementation
        import time
        time.sleep(1.0)
        follower_config = self.conn_base_config + 'disaggregated=(role="follower")'
        self.reopen_conn(config=follower_config)
        time.sleep(1.0)

        cursor = self.session.open_cursor(uri, None, None)

        for i in range(self.nitems):
            self.assertEqual(cursor["Hello " + str(i)], "World")
