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

import wiredtiger, wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered76.py
# Checkpoint size verification

@disagg_test_class
class test_layered76(wttest.WiredTigerTestCase):
    conn_config = 'disaggregated=(role="leader")'

    create_session_config = 'key_format=i,value_format=S'

    uri = "layered:test_layered76"

    disagg_storages = gen_disagg_storages('test_layered66', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    def test_ckpt_size_verify_simple(self):
        self.session.create(self.uri, self.create_session_config)

        # Insert a key.
        cursor = self.session.open_cursor(self.uri)
        cursor[1] = 'value1'
        cursor.close()

        # Do a checkpoint.
        self.session.checkpoint()

        self.verifyUntilSuccess()

    def test_ckpt_size_verify_multi_insert(self):
        self.session.create(self.uri, self.create_session_config)

        # Insert data.
        cursor = self.session.open_cursor(self.uri)
        for i in range(10):
            cursor[i] = 'a' * 100
        cursor.close()

        # Do a checkpoint.
        self.session.checkpoint()

        self.verifyUntilSuccess()

    def test_ckpt_size_verify_large_dataset(self):
        self.session.create(self.uri, self.create_session_config)

        # Insert data.
        cursor = self.session.open_cursor(self.uri)
        for i in range(100000):
            cursor[i] = 'a' * 100
        cursor.close()

        # Do a checkpoint.
        self.session.checkpoint()

        self.verifyUntilSuccess()

    def test_ckpt_size_verify_many_ckpt(self):
        session_config = 'key_format=S,value_format=S'
        nitems = 10000

        self.session.create(self.uri, session_config)

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(nitems):
            cursor["Key " + str(i)] = str(i)
        cursor.close()

        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(nitems):
            if i % 2 == 0:
                cursor["Key " + str(i)] = str(i) + "_even"
        cursor.close()

        self.session.checkpoint()

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(nitems):
            if i % 100 == 0:
                cursor["Key " + str(i)] = str(i) + "_hundred"
        cursor.close()

        self.session.checkpoint()

        self.verifyUntilSuccess()
