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

import threading, time, wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered68.py
#    Test that address cookies in disaggregated storage can be upgraded/downgraded safely.
@disagg_test_class
class test_layered68(wttest.WiredTigerTestCase):
    conn_base_config = 'statistics=(all),' \
                     + 'statistics_log=(wait=1,json=true,on_close=true),' \
                     + 'precise_checkpoint=true,'
    conn_config = conn_base_config + 'disaggregated=(role="follower"),'

    create_session_config = 'key_format=S,value_format=S,type=layered'

    num_items = 2000
    num_modify = 100
    uri = "table:test_layered68"

    address_cookie_upgrade = [
        ('none', dict(address_cookie_upgrade='none', compatible=True)),
        ('compatible', dict(address_cookie_upgrade='compatible', compatible=True)),
        ('incompatible', dict(address_cookie_upgrade='incompatible', compatible=False)),
    ]
    optional_field = [
        ('none', dict(optional_field='false')),
        ('optional_field', dict(optional_field='true')),
    ]

    disagg_storages = gen_disagg_storages('test_layered68', disagg_only = True)
    scenarios = make_scenarios(disagg_storages, address_cookie_upgrade, optional_field)

    # Test stepping up concurrently with a checkpoint.
    def test_layered68(self):
        self.conn.reconfigure('disaggregated=(role="leader")')

        self.session.create(self.uri, self.create_session_config)

        # Create table with some data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.num_items):
            cursor[f'key{i:04}'] = f'value{i:04}' + 'abcd' * 100
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(1))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(1))
        self.session.checkpoint()

        #
        # Part 1: Start a node with the newer version of address cookies.
        #
        debug_mode = f'disagg_address_cookie_optional_field={self.optional_field},' \
                     f'disagg_address_cookie_upgrade={self.address_cookie_upgrade}'
        self.restart_without_local_files(config=self.conn_config + f',debug_mode=({debug_mode})')

        # Check that all the data is present.
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.num_items):
            key = f'key{i:04}'
            expected_value = f'value{i:04}' + 'abcd' * 100
            self.assertEqual(cursor[key], expected_value)
        cursor.close()

        # Step up.
        self.conn.reconfigure('disaggregated=(role="leader")')

        # Modify some data.
        self.session.begin_transaction()
        cursor = self.session.open_cursor(self.uri, None, None)
        num_modified = self.num_modify
        for i in range(0, num_modified):
            key = f'key{i:04}'
            cursor[key] = f'value_mod{i:04}' + 'abcd' * 100
        cursor.close()
        self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

        self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
        self.session.checkpoint()

        #
        # Part 2: Start a node with the older version of address cookies.
        #
        self.conn.reconfigure('disaggregated=(role="follower")') # Avoid shutdown checkpoint
        checkpoint_meta = self.disagg_get_complete_checkpoint_meta()
        self.restart_without_local_files(pickup_checkpoint=False,
            config=self.conn_config + ',debug_mode=(disagg_address_cookie_upgrade=none)')

        # Pickup the latest checkpoint.
        if self.compatible:
            self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")')
        else:
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.conn.reconfigure(f'disaggregated=(checkpoint_meta="{checkpoint_meta}")'),
                '/Unsupported disaggregated address cookie version/')

        # If compatible, then check if the data is present and make more changes. Otherwise,
        # we are done.
        if self.compatible:

            # Check that all the data is present.
            cursor = self.session.open_cursor(self.uri, None, None)
            for i in range(self.num_items):
                key = f'key{i:04}'
                prefix = 'value_mod' if i < self.num_modify else 'value'
                expected_value = f'{prefix}{i:04}' + 'abcd' * 100
                self.assertEqual(cursor[key], expected_value)
            cursor.close()

            # Step up.
            self.conn.reconfigure('disaggregated=(role="leader")')

            # Modify some data to make sure we can keep writing.
            self.session.begin_transaction()
            cursor = self.session.open_cursor(self.uri, None, None)
            num_modified = self.num_modify * 2
            for i in range(self.num_modify, num_modified):
                key = f'key{i:04}'
                cursor[key] = f'value_mod{i:04}' + 'abcd' * 100
            cursor.close()
            self.session.commit_transaction('commit_timestamp=' + self.timestamp_str(2))

            self.conn.set_timestamp('stable_timestamp=' + self.timestamp_str(2))
            self.session.checkpoint()

        #
        # Part 3: Restart a node with the newer version of address cookies.
        #
        self.restart_without_local_files(config=self.conn_config + f',debug_mode=({debug_mode})')

        # Check that all the data is present.
        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.num_items):
            key = f'key{i:04}'
            prefix = 'value_mod' if i < num_modified else 'value'
            expected_value = f'{prefix}{i:04}' + 'abcd' * 100
            self.assertEqual(cursor[key], expected_value)
        cursor.close()
