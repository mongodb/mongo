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

import os
import wiredtiger
import wttest
from helper_disagg import disagg_test_class, gen_disagg_storages
from wtscenario import make_scenarios

# test_layered22.py
# Test a secondary can perform reads and writes to the ingest component
# of a layered table, without the stable component.
@disagg_test_class
class test_layered22(wttest.WiredTigerTestCase):
    conn_base_config = 'transaction_sync=(enabled,method=fsync),'

    disagg_storages = gen_disagg_storages('test_layered22', disagg_only = True)
    scenarios = make_scenarios(disagg_storages)

    nitems = 10000
    uri = 'layered:test_layered22'

    def conn_config(self):
        return self.conn_base_config + 'disaggregated=(role="follower"),'

    def session_create_config(self):
        return 'key_format=S,value_format=S,'

    def test_secondary_reads_without_stable(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor["Hello " + str(i)] = "World"
            cursor["Hi " + str(i)] = "There"
            cursor["OK " + str(i)] = "Go"
        cursor.close()

        cursor = self.session.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor.next() == 0:
            item_count += 1
        self.assertEqual(item_count, self.nitems * 3)
        cursor.close()

        # Test cursor->prev as well
        cursor = self.session.open_cursor(self.uri, None, None)
        item_count = 0
        while cursor.prev() == 0:
            item_count += 1
        self.assertEqual(item_count, self.nitems * 3)
        cursor.close()

    def test_secondary_modifies_without_stable(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        value1 = "aaaa"
        value2 = "abaa"

        for i in range(self.nitems):
            cursor[str(i)] = value1

        for i in range(self.nitems):
            if i % 10 == 0:
                self.session.begin_transaction()
                cursor.set_key(str(i))
                mods = [wiredtiger.Modify('b', 1, 1)]
                self.assertEqual(cursor.modify(mods), 0)
                self.session.commit_transaction()

        cursor.close()

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            if i % 10 == 0:
                self.assertEqual(cursor[str(i)], value2)
            else:
                self.assertEqual(cursor[str(i)], value1)
        cursor.close()

    def test_secondary_search_without_stable(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)

        cursor.set_key("nonexistent")
        self.assertEqual(cursor.search(), wiredtiger.WT_NOTFOUND)
        self.assertEqual(cursor.search_near(), wiredtiger.WT_NOTFOUND)

        cursor["found"] = "yes"
        cursor.set_key("found")
        self.assertEqual(cursor.search(), 0)
        self.assertEqual(cursor.search_near(), 0)

    def test_largest_key_without_stable(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor["Hello " + str(i)] = "World"
            cursor["Hi " + str(i)] = "There"
            cursor["OK " + str(i)] = "Go"
        cursor.close()

        cursor = self.session.open_cursor(self.uri, None, None)
        self.assertEqual(cursor.largest_key(), 0)
        self.assertEqual(cursor.get_key(), "OK " + str(self.nitems - 1))

    def test_getrandom_without_stable(self):
        self.session.create(self.uri, self.session_create_config())

        cursor = self.session.open_cursor(self.uri, None, None)
        for i in range(self.nitems):
            cursor["Hello " + str(i)] = "World"
        cursor.close()

        random_cursor = self.session.open_cursor(self.uri, None, "next_random=true")
        self.assertEqual(random_cursor.next(), 0)
        self.assertTrue(random_cursor.get_key().startswith("Hello "))
