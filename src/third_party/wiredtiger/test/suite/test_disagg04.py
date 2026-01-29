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
from helper_disagg import DisaggConfigMixin, gen_disagg_storages
from wiredtiger import stat

# test_disagg04.py
# Note that the APIs we are testing are not meant to be used directly
# by any WiredTiger application, these APIs are used internally.
# However, it is useful to do tests of this API independently.

class test_disagg04(wttest.WiredTigerTestCase, DisaggConfigMixin):

    disagg_storages = gen_disagg_storages('test_disagg04', disagg_only = True)

    uri = "layered:test_disagg04_%02d"
    cold_table_config = 'key_format=S,value_format=S,disaggregated=(storage_tier=cold),'

    # Load the storage store extension.
    def conn_extensions(self, extlist):
        DisaggConfigMixin.conn_extensions(self, extlist)

    def validate_config(self, uri, config_str, check_func=None):
        self.session.create(uri, config_str)

        self.reopen_conn()
        c = self.session.open_cursor('metadata:', None, None)
        c.set_key(uri)
        self.assertNotEqual(c.search(), wiredtiger.WT_NOTFOUND)
        if check_func is not None:
            check_func(c.get_value())
        c.close()

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def add_data(self, uri, nitems):
        cursor = self.session.open_cursor(uri, None, None)
        for i in range(nitems):
            cursor["Key " + str(i)] = str(i)
        cursor.close()
        self.session.checkpoint()


    def test_disagg_storage_tier(self):
        self.conn.reconfigure(f'disaggregated=(role=leader)')

        # Test invalid storage_tier value (empty)
        with self.expectedStderrPattern('Invalid argument'):
            with self.assertRaises(wiredtiger.WiredTigerError):
                self.validate_config(
                    self.uri%1,
                    'key_format=S,value_format=S,disaggregated=(storage_tier=),'
                )

        # Test no storage_tier specified: when this option is not specified in the config,
        # it will not be included in the metadata string. This keeps backward compatibility with
        # all existing test cases, config patterns, and existing databases.
        self.validate_config(
            self.uri%2,
            'key_format=S,value_format=S,',
            lambda v: self.assertTrue(v.find('storage_tier=') == -1)
        )

        # Test valid storage_tier configuration value (cold)
        self.validate_config(
            self.uri%3,
            self.cold_table_config,
            lambda v: self.assertTrue(v.find('storage_tier=cold') != -1)
        )

        # Test invalid storage_tier value (typo)
        with self.expectedStderrPattern('Invalid argument'):
            with self.assertRaises(wiredtiger.WiredTigerError):
                self.validate_config(
                    self.uri%4,
                    'key_format=S,value_format=S,disaggregated=(storage_tier=coldd),',
                    lambda v: self.assertTrue(v.find('storage_tier=cold') == -1)
                )

    def test_cold_write(self):
        self.conn.reconfigure(f'disaggregated=(role=leader)')

        uri = self.uri%5

        self.session.create(uri, self.cold_table_config)

        self.assertEqual(self.get_stat(stat.conn.disagg_block_put_cold), 0)

        self.add_data(uri, 1000)

        self.assertGreater(self.get_stat(stat.conn.disagg_block_put_cold), 0)

    def test_cold_read(self):
        self.conn.reconfigure('disaggregated=(role=leader)')

        uri = self.uri%6

        self.session.create(uri, self.cold_table_config)

        self.add_data(uri, 1000)

        self.assertEqual(self.get_stat(stat.conn.disagg_block_get_cold), 0)

        # Verify the table to read all pages.
        self.verifyUntilSuccess(uri=uri)

        self.assertGreater(self.get_stat(stat.conn.disagg_block_get_cold), 0)
