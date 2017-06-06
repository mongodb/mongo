#!/usr/bin/env python
#
# Public Domain 2014-2017 MongoDB, Inc.
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
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_inmem01.py
#    Test in-memory configuration.
class test_inmem01(wttest.WiredTigerTestCase):
    uri = 'table:inmem01'
    conn_config = \
        'cache_size=5MB,file_manager=(close_idle_time=0),in_memory=true'
    table_config = ',memory_page_max=32k,leaf_page_max=4k'

    scenarios = make_scenarios([
        ('col', dict(keyfmt='r', valuefmt='S')),
        ('fix', dict(keyfmt='r', valuefmt='8t')),
        ('row', dict(keyfmt='S', valuefmt='S')),
    ])

    # Smoke-test in-memory configurations, add a small amount of data and
    # ensure it's visible.
    def test_insert(self):
        ds = SimpleDataSet(self, self.uri, 1000, key_format=self.keyfmt,
            value_format=self.valuefmt, config=self.table_config)
        ds.populate()
        ds.check()

    # Add more data than fits into the configured cache and verify it fails.
    def test_insert_over_capacity(self):
        msg = '/WT_CACHE_FULL.*/'
        ds = SimpleDataSet(self, self.uri, 10000000, key_format=self.keyfmt,
            value_format=self.valuefmt, config=self.table_config)
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            ds.populate, msg)

        # Figure out the last key we successfully inserted, and check all
        # previous inserts are still there.
        cursor = self.session.open_cursor(self.uri, None)
        cursor.prev()
        last_key = int(cursor.get_key())
        ds = SimpleDataSet(self, self.uri, last_key, key_format=self.keyfmt,
            value_format=self.valuefmt, config=self.table_config)
        ds.check()

    # Fill the cache with data, remove some data, ensure more data can be
    # inserted (after a reasonable amount of time for space to be reclaimed).
    def test_insert_over_delete(self):
        msg = '/WT_CACHE_FULL.*/'
        ds = SimpleDataSet(self, self.uri, 10000000, key_format=self.keyfmt,
            value_format=self.valuefmt, config=self.table_config)
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            ds.populate, msg)

        # Now that the database contains as much data as will fit into
        # the configured cache, verify removes succeed.
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(1, 100):
            cursor.set_key(ds.key(i))
            cursor.remove()

    # Run queries after adding, removing and re-inserting data.
    # Try out keeping a cursor open while adding new data.
    def test_insert_over_delete_replace(self):
        msg = '/WT_CACHE_FULL.*/'
        ds = SimpleDataSet(self, self.uri, 10000000, key_format=self.keyfmt,
            value_format=self.valuefmt, config=self.table_config)
        self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
            ds.populate, msg)

        cursor = self.session.open_cursor(self.uri, None)
        cursor.prev()
        last_key = int(cursor.get_key())

        # Now that the database contains as much data as will fit into
        # the configured cache, verify removes succeed.
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(1, last_key / 4, 1):
            cursor.set_key(ds.key(i))
            cursor.remove()

        cursor.reset()
        # Spin inserting to give eviction a chance to reclaim space
        sleeps = 0
        inserted = False
        for i in range(1, 1000):
            try:
                cursor[ds.key(1)] = ds.value(1)
            except wiredtiger.WiredTigerError:
                cursor.reset()
                sleeps = sleeps + 1
                self.assertLess(sleeps, 60 * 5)
                sleep(1)
                continue
            inserted = True
            break
        self.assertTrue(inserted)

    # Custom "keep filling" helper
    def fill(self, cursor, ds, start, end):
        for i in xrange(start + 1, end + 1):
            cursor[ds.key(i)] = ds.value(i)

    # Keep adding data to the cache until it becomes really full, make sure
    # that reads aren't blocked.
    @wttest.longtest("Try to wedge an in-memory cache")
    def test_wedge(self):
        # Try to really wedge the cache full
        ds = SimpleDataSet(self, self.uri, 0, key_format=self.keyfmt,
            value_format=self.valuefmt, config=self.table_config)
        ds.populate()
        cursor = self.session.open_cursor(self.uri, None)

        run = 0
        start, last_key = -1000, 0
        while last_key - start > 100:
            msg = '/WT_CACHE_FULL.*/'
            start = last_key
            self.assertRaisesHavingMessage(wiredtiger.WiredTigerError,
                lambda: self.fill(cursor, ds, start, 10000000), msg)
            cursor.reset()
            sleep(1)

            # Figure out the last key we successfully inserted, and check all
            # previous inserts are still there.
            cursor.prev()
            last_key = int(cursor.get_key())
            run += 1
            self.pr('Finished iteration ' + str(run) + ', last_key = ' + str(last_key))

        self.pr('Checking ' + str(last_key) + ' keys')
        ds = SimpleDataSet(self, self.uri, last_key, key_format=self.keyfmt,
            value_format=self.valuefmt, config=self.table_config)

        # This test is *much* slower for fixed-length column stores: we fit
        # many more records into the cache, so don't do as many passes through
        # the data.
        checks = 10 if self.valuefmt.endswith('t') else 100
        for run in xrange(checks):
            ds.check()
            self.pr('Finished check ' + str(run))
            sleep(1)

if __name__ == '__main__':
    wttest.run()
