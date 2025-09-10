#!/usr/bin/env python3
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

import os, random, string, wttest
from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios
from wiredtiger import stat

# test_layered48.py
#    Ensure overflow keys and values are not being generated in disaggregated storage.

@disagg_test_class
class test_layered48(wttest.WiredTigerTestCase, DisaggConfigMixin):
    nitems = 500
    key_to_update = 0
    num_updates = 10

    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,disaggregated=(page_log=palm),'
    conn_config = conn_base_config + 'disaggregated=(role="leader")'

    create_session_config = 'key_format=S,value_format=S,leaf_key_max=256,leaf_value_max=256'

    table_name = "test_layered48"

    disagg_storages = gen_disagg_storages('test_layered48', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, [
        ('layered', dict(prefix='layered:')),
        ('shared', dict(prefix='table:')),
    ])

    # Load the page log extension, which has object storage support
    def conn_extensions(self, extlist):
        if os.name == 'nt':
            extlist.skip_if_missing = True
        DisaggConfigMixin.conn_extensions(self, extlist)

    # Custom test case setup
    def early_setup(self):
        os.mkdir('follower')
        # Create the home directory for the PALM k/v store, and share it with the follower.
        os.mkdir('kv_home')
        os.symlink('../kv_home', 'follower/kv_home', target_is_directory=True)

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def generate_random_string(self, length):
        characters = string.ascii_letters + string.digits + string.punctuation
        random_string = ''.join(random.choices(characters, k=length))
        return random_string

    # Test overflow keys and values.
    def test_layered48(self):

        # Create table
        self.uri = self.prefix + self.table_name
        table_config = self.create_session_config
        if not self.uri.startswith('layered'):
            table_config += ',block_manager=disagg,log=(enabled=false)'
        self.session.create(self.uri, table_config)

        # Put big data to the table
        key_prefix1 = self.generate_random_string(1000)
        value_prefix1 = 'matcha'
        timestamp1 = 100

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            # Don't make the transaction too long due to eviction hangs.
            self.session.begin_transaction()
            cursor[key_prefix1 + str(i)] = value_prefix1 + str(i)
            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp1)}')
        cursor.close()

        # Assert that no overflow values were generated.
        self.assertEqual(self.get_stat(stat.conn.rec_overflow_key_leaf), 0)
        self.assertEqual(self.get_stat(stat.conn.rec_overflow_value), 0)

        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp1)}')
        self.session.checkpoint()

        # Create several updates with big values.
        timestamp2 = 200
        value_prefix2 = self.generate_random_string(1000)
        for n in range(1, self.num_updates):
            self.session.begin_transaction()
            cursor = self.session.open_cursor(self.uri, None, None)
            cursor[key_prefix1 + str(n * 100)] = value_prefix2  + '-' + str(n)
            cursor.close()

            self.session.commit_transaction(f'commit_timestamp={self.timestamp_str(timestamp2)}')
        self.conn.set_timestamp(f'stable_timestamp={self.timestamp_str(timestamp2)}')
        self.session.checkpoint()

        # Assert that no overflow values were generated.
        self.assertEqual(self.get_stat(stat.conn.rec_overflow_key_leaf), 0)
        self.assertEqual(self.get_stat(stat.conn.rec_overflow_value), 0)
