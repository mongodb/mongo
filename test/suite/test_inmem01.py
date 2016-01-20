#!/usr/bin/env python
#
# Public Domain 2014-2016 MongoDB, Inc.
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
from time import sleep
from helper import simple_populate, simple_populate_check
from helper import key_populate, value_populate
from wtscenario import check_scenarios

# test_inmem01.py
#    Test in-memory configuration.
class test_inmem01(wttest.WiredTigerTestCase):
    name = 'inmem01'
    """
    In memory configuration still creates files on disk, but has limits
    in terms of how much data can be written.
    Test various scenarios including:
     - Add a small amount of data, ensure it is present.
     - Add more data than would fit into the configured cache.
     - Fill the cache with data, remove some data, ensure more data can be
       inserted (after a reasonable amount of time for space to be reclaimed)
     - Run queries after adding, removing and re-inserting data.
     - Try out keeping a cursor open while adding new data.
    """
    scenarios = check_scenarios([
        ('col', dict(tablekind='col')),
        # Fixed length is very slow, disable it for now
        #('fix', dict(tablekind='fix')),
        ('row', dict(tablekind='row'))
    ])

    # create an in-memory database
    conn_config = 'cache_size=5MB,' + \
                  'file_manager=(close_idle_time=0),in_memory=true'

    def get_table_config(self):
        kf = 'key_format='
        vf = 'value_format='
        if self.tablekind == 'row':
            kf = kf + 'S'
        else:
            kf = kf + 'r'  # record format
        if self.tablekind == 'fix':
            vf = vf + '8t'
        else:
            vf = vf + 'S'
        return 'memory_page_max=32k,leaf_page_max=4k,' + kf + ',' + vf

    def test_insert(self):
        table_config = self.get_table_config()
        simple_populate(self,
            "table:" + self.name, table_config, 1000)
        # Ensure the data is visible.
        simple_populate_check(self, 'table:' + self.name, 1000)

    def test_insert_over_capacity(self):
        table_config = self.get_table_config()
        msg = '/WT_CACHE_FULL.*/'
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda:simple_populate(self,
                "table:" + self.name, table_config, 10000000), msg)

        # Figure out the last key we inserted.
        cursor = self.session.open_cursor('table:' + self.name, None)
        cursor.prev()
        last_key = int(cursor.get_key())
        simple_populate_check(self, 'table:' + self.name, last_key)

    def test_insert_over_delete(self):
        table_config = self.get_table_config()
        msg = '/WT_CACHE_FULL.*/'
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda:simple_populate(self,
                "table:" + self.name, table_config, 10000000), msg)

        # Now that the database contains as much data as will fit into
        # the configured cache, verify removes succeed.
        cursor = self.session.open_cursor('table:' + self.name, None)
        for i in range(1, 100):
            cursor.set_key(key_populate(cursor, i))
            cursor.remove()

    def test_insert_over_delete_replace(self):
        table_config = self.get_table_config()
        msg = '/WT_CACHE_FULL.*/'
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            lambda:simple_populate(self,
                "table:" + self.name, table_config, 10000000), msg)

        cursor = self.session.open_cursor('table:' + self.name, None)
        cursor.prev()
        last_key = int(cursor.get_key())

        # Now that the database contains as much data as will fit into
        # the configured cache, verify removes succeed.
        cursor = self.session.open_cursor('table:' + self.name, None)
        for i in range(1, last_key / 4, 1):
            cursor.set_key(key_populate(cursor, i))
            cursor.remove()

        cursor.reset()
        # Spin inserting to give eviction a chance to reclaim space
        inserted = False
        for i in range(1, 1000):
            try:
                cursor[key_populate(cursor, 1)] = value_populate(cursor, 1)
            except wiredtiger.WiredTigerError:
                cursor.reset()
                sleep(1)
                continue
            inserted = True
            break
        self.assertTrue(inserted)

if __name__ == '__main__':
    wttest.run()
