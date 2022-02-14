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
#
# [TEST_TAGS]
# cursors:prepare
# [END_TAGS]

import wiredtiger, wttest
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_prepare_cursor01.py
#    WT_CURSOR navigation (next/prev) tests with prepared transactions
class test_prepare_cursor01(wttest.WiredTigerTestCase):

    fmt = [
        ('row-store', dict(keyfmt='i', valfmt='S')),
        ('column-store', dict(keyfmt='r', valfmt='S')),
        ('fixed-length-column-store', dict(keyfmt='r', valfmt='8t')),
    ]
    types = [
        ('table-simple', dict(uri='table', ds=SimpleDataSet)),
    ]

    iso_types = [
        ('isolation_read_committed', dict(isolation='read-committed')),
        ('isolation_snapshot', dict(isolation='snapshot'))
    ]

    def keep(name, d):
        return d['keyfmt'] != 'r' or (d['uri'] != 'lsm' and not d['ds'].is_lsm())

    scenarios = make_scenarios(types, fmt, iso_types, include=keep)

    # Test cursor navigate (next/prev) with prepared transactions.
    # Cursor navigate with timestamp reads and non-timestamped reads.
    #   before cursor  : with timestamp earlier to prepare timestamp.
    #   between cursor : with timestamp between prepare and commit timestamps.
    #   after cursor   : with timestamp after commit timestamp.
    # Cursor with out read timestamp behaviour should be same after cursor behavior.
    def test_cursor_navigate_prepare_transaction(self):

        # Build an object.
        uri = self.uri + ':test_prepare_cursor01'
        ds = self.ds(self, uri, 50, key_format=self.keyfmt)
        ds.populate()

        # Session for non-timestamped reads.
        session = self.conn.open_session()
        cursor = session.open_cursor(uri, None)
        cursor.set_key(ds.key(1))
        cursor.remove()

        # Session for timestamped reads before prepare timestamp.
        before_ts_s = self.conn.open_session()
        before_ts_c = before_ts_s.open_cursor(uri, None)
        # Session for timestamped reads between prepare timestamp and commit timestamp.
        between_ts_s = self.conn.open_session()
        between_ts_c = between_ts_s.open_cursor(uri, None)
        # Session for timestamped reads after commit timestamp.
        after_ts_s = self.conn.open_session()
        after_ts_c = after_ts_s.open_cursor(uri, None)

        prep_session = self.conn.open_session('isolation=snapshot')
        prep_cursor = prep_session.open_cursor(uri, None)

        # Scenario-1 : Check cursor navigate with insert in prepared transaction.
        # Begin of Scenario-1.
        # Data set at start has keys {2,3,4 ... 50}
        # Insert key 51 to check next operation.
        # Insert key 1 to check prev operation.
        prep_session.begin_transaction()
        prep_cursor.set_key(ds.key(51))
        prep_cursor.set_value(ds.value(51))
        prep_cursor.insert()
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100))

        # Point all cursors to key 50.
        before_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(50))
        before_ts_c.set_key(ds.key(50))
        self.assertEquals(before_ts_c.search(), 0)

        between_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(150))
        between_ts_c.set_key(ds.key(50))
        self.assertEquals(between_ts_c.search(), 0)

        after_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(250))
        after_ts_c.set_key(ds.key(50))
        self.assertEquals(after_ts_c.search(), 0)

        session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(ds.key(50))
        self.assertEquals(cursor.search(), 0)

        # Check the visibility of newly inserted, prepared update.

        # As read is before prepare timestamp, next is not found.
        self.assertEquals(before_ts_c.next(), wiredtiger.WT_NOTFOUND)
        # As read is between, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.next())
        # Check to see prev works when a next returns prepare conflict.
        self.assertEquals(between_ts_c.prev(), 0)
        self.assertEquals(between_ts_c.get_key(), ds.key(50))
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.next())
        # As read is after, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: after_ts_c.next())
        # As read is non-timestamped, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.next())

        # Commit the prepared transaction.
        prep_session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(200))
        prep_session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(200))
        prep_session.commit_transaction()

        before_ts_s.commit_transaction()
        # As read is between(i.e before commit), next is not found.
        self.assertEquals(between_ts_c.next(), wiredtiger.WT_NOTFOUND)
        between_ts_s.commit_transaction()
        # As read is after, next will point to new key 51
        self.assertEquals(after_ts_c.next(), 0)
        self.assertEquals(after_ts_c.get_key(), ds.key(51))
        after_ts_s.commit_transaction()
        # Non-timestamped read should find new key 51.
        self.assertEquals(cursor.next(), 0)
        self.assertEquals(cursor.get_key(), ds.key(51))
        session.commit_transaction()

        # Insert key 1 to check prev operation.
        prep_session.begin_transaction()
        prep_cursor.set_key(ds.key(1))
        prep_cursor.set_value(ds.value(1))
        prep_cursor.insert()
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(100))

        # Point all cursors to key 2.
        before_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(50))
        before_ts_c.set_key(ds.key(2))
        self.assertEquals(before_ts_c.search(), 0)

        between_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(150))
        between_ts_c.set_key(ds.key(2))
        self.assertEquals(between_ts_c.search(), 0)

        after_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(250))
        after_ts_c.set_key(ds.key(2))
        self.assertEquals(after_ts_c.search(), 0)

        session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(ds.key(2))
        self.assertEquals(cursor.search(), 0)

        # Check the visibility of newly inserted, prepared update.

        # As read is before prepare timestamp, prev is not found.
        self.assertEquals(before_ts_c.prev(), wiredtiger.WT_NOTFOUND)
        before_ts_s.commit_transaction()
        # As read is between, prev will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.prev())
        # Check to see next works when a prev returns prepare conflict.
        self.assertEquals(between_ts_c.next(), 0)
        self.assertEquals(between_ts_c.get_key(), ds.key(2))
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.prev())
        # As read is after, prev will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: after_ts_c.prev())
        # As read is non-timestamped, prev will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.prev())

        # Commit the prepared transaction.
        prep_session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(200))
        prep_session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(200))
        prep_session.commit_transaction()

        # As read is between(i.e before commit), prev is not found.
        self.assertEquals(between_ts_c.prev(), wiredtiger.WT_NOTFOUND)
        between_ts_s.commit_transaction()
        # As read is after, prev will point to new key 1.
        self.assertEquals(after_ts_c.prev(), 0)
        self.assertEquals(after_ts_c.get_key(), ds.key(1))
        after_ts_s.commit_transaction()
        # Non-timestamped read should find new key 1.
        self.assertEquals(cursor.prev(), 0)
        self.assertEquals(cursor.get_key(), ds.key(1))
        session.commit_transaction()

        # End of Scenario-1.

        # Scenario-2 : Check cursor navigate with update in prepared transaction.
        # Begin of Scenario-2.
        # Data set at start has keys {1,2,3,4 ... 50,51}
        # Update key 51 to check next operation.
        # Update key 1 to check prev operation.
        prep_session.begin_transaction()
        prep_cursor.set_key(ds.key(51))
        prep_cursor.set_value(ds.value(151))
        prep_cursor.update()
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(300))

        # Point all cursors to key 51.
        before_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(250))
        before_ts_c.set_key(ds.key(50))
        self.assertEquals(before_ts_c.search(), 0)

        between_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(350))
        between_ts_c.set_key(ds.key(50))
        self.assertEquals(between_ts_c.search(), 0)

        after_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(450))
        after_ts_c.set_key(ds.key(50))
        self.assertEquals(after_ts_c.search(), 0)

        session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(ds.key(50))
        self.assertEquals(cursor.search(), 0)

        # Check the visibility of newly inserted, prepared update.

        # As read is before prepare timestamp, next is found with previous value.
        self.assertEquals(before_ts_c.next(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(51))
        self.assertEquals(before_ts_c.get_value(), ds.value(51))
        # As read is between, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.next())
        # As read is after, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: after_ts_c.next())
        # As read is non-timestamped, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.next())

        # Commit the prepared transaction.
        prep_session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(400))
        prep_session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(400))
        prep_session.commit_transaction()

        # Check to see before cursor still gets the old value.
        before_ts_c.set_key(ds.key(51))
        self.assertEquals(before_ts_c.search(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(51))
        self.assertEquals(before_ts_c.get_value(), ds.value(51))
        before_ts_s.commit_transaction()
        # As read is between(i.e before commit), next is not found.
        self.assertEquals(between_ts_c.next(), 0)
        self.assertEquals(between_ts_c.get_key(), ds.key(51))
        self.assertEquals(between_ts_c.get_value(), ds.value(51))
        between_ts_s.commit_transaction()
        # As read is after, next will point to new key 51.
        self.assertEquals(after_ts_c.next(), 0)
        self.assertEquals(after_ts_c.get_key(), ds.key(51))
        self.assertEquals(after_ts_c.get_value(), ds.value(151))
        after_ts_s.commit_transaction()
        # Non-timestamped read should find new key 51.
        self.assertEquals(cursor.next(), 0)
        self.assertEquals(cursor.get_key(), ds.key(51))
        self.assertEquals(cursor.get_value(), ds.value(151))
        session.commit_transaction()

        # Update key 1 to check prev operation.
        prep_session.begin_transaction()
        prep_cursor.set_key(ds.key(1))
        prep_cursor.set_value(ds.value(111))
        prep_cursor.update()
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(300))

        # Point all cursors to key 2.
        before_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(250))
        before_ts_c.set_key(ds.key(2))
        self.assertEquals(before_ts_c.search(), 0)

        between_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(350))
        between_ts_c.set_key(ds.key(2))
        self.assertEquals(between_ts_c.search(), 0)

        after_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(450))
        after_ts_c.set_key(ds.key(2))
        self.assertEquals(after_ts_c.search(), 0)

        session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(ds.key(2))
        self.assertEquals(cursor.search(), 0)

        # Check the visibility of new update of prepared transaction.

        # As read is before prepare timestamp, prev is not found.
        self.assertEquals(before_ts_c.prev(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(1))
        self.assertEquals(before_ts_c.get_value(), ds.value(1))
        # As read is between, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.prev())
        # As read is after, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: after_ts_c.prev())
        # As read is non-timestamped, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.prev())

        # Commit the prepared transaction.
        prep_session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(400))
        prep_session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(400))
        prep_session.commit_transaction()

        # Check to see before cursor still gets the old value.
        before_ts_c.set_key(ds.key(1))
        self.assertEquals(before_ts_c.search(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(1))
        self.assertEquals(before_ts_c.get_value(), ds.value(1))
        before_ts_s.commit_transaction()
        # As read is between(i.e before commit), prev should get old value.
        self.assertEquals(between_ts_c.prev(), 0)
        self.assertEquals(between_ts_c.get_value(), ds.value(1))
        between_ts_s.commit_transaction()
        # As read is after, prev should get new value.
        self.assertEquals(after_ts_c.prev(), 0)
        self.assertEquals(after_ts_c.get_key(), ds.key(1))
        self.assertEquals(after_ts_c.get_value(), ds.value(111))
        after_ts_s.commit_transaction()
        # Non-timestamped read should find new key 1.
        self.assertEquals(cursor.prev(), 0)
        self.assertEquals(cursor.get_key(), ds.key(1))
        self.assertEquals(cursor.get_value(), ds.value(111))
        session.commit_transaction()

        # End of Scenario-2.

        # Scenario-3 : Check cursor navigate with remove in prepared transaction.
        # Begin of Scenario-3.
        # Data set at start has keys {1,2,3,4 ... 50,51}
        # Remove key 51 to check next operation.
        # Remove key 1 to check prev operation.
        prep_session.begin_transaction()
        prep_cursor.set_key(ds.key(51))
        prep_cursor.remove()
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(500))

        # Point all cursors to key 51.
        before_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(450))
        before_ts_c.set_key(ds.key(50))
        self.assertEquals(before_ts_c.search(), 0)

        between_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(550))
        between_ts_c.set_key(ds.key(50))
        self.assertEquals(between_ts_c.search(), 0)

        after_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(650))
        after_ts_c.set_key(ds.key(50))
        self.assertEquals(after_ts_c.search(), 0)

        session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(ds.key(50))
        self.assertEquals(cursor.search(), 0)

        # Check the visibility of removed prepared update.

        # As read is before prepare timestamp, next is found with key 51.
        self.assertEquals(before_ts_c.next(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(51))
        # As read is between, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.next())
        # As read is after, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: after_ts_c.next())
        # As read is non-timestamped, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.next())

        # Commit the prepared transaction.
        prep_session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(600))
        prep_session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(600))
        prep_session.commit_transaction()

        # Check to see before cursor still gets the old value.
        before_ts_c.set_key(ds.key(51))
        self.assertEquals(before_ts_c.search(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(51))
        before_ts_s.commit_transaction()
        # As read is between(i.e before commit), next is not found.
        self.assertEquals(between_ts_c.next(), 0)
        self.assertEquals(between_ts_c.get_key(), ds.key(51))
        between_ts_s.commit_transaction()
        # As read is after, next will point beyond end.
        self.assertEquals(after_ts_c.next(), wiredtiger.WT_NOTFOUND)
        after_ts_s.commit_transaction()
        # Non-timestamped read should not find a key.
        self.assertEquals(cursor.next(), wiredtiger.WT_NOTFOUND)
        session.commit_transaction()

        # Remove key 1 to check prev operation.
        prep_session.begin_transaction()
        prep_cursor.set_key(ds.key(1))
        prep_cursor.remove()
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(500))

        # Point all cursors to key 2.
        before_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(450))
        before_ts_c.set_key(ds.key(2))
        self.assertEquals(before_ts_c.search(), 0)

        between_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(550))
        between_ts_c.set_key(ds.key(2))
        self.assertEquals(between_ts_c.search(), 0)

        after_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(650))
        after_ts_c.set_key(ds.key(2))
        self.assertEquals(after_ts_c.search(), 0)

        session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(ds.key(2))
        self.assertEquals(cursor.search(), 0)

        # Check the visibility of new update of prepared transaction.

        # As read is before prepare timestamp, prev is not found.
        self.assertEquals(before_ts_c.prev(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(1))
        # As read is between, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.prev())
        # As read is after, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: after_ts_c.prev())
        # As read is non-timestamped, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.prev())

        # Commit the prepared transaction.
        prep_session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(600))
        prep_session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(600))
        prep_session.commit_transaction()

        # Check to see before cursor still gets the old value.
        before_ts_c.set_key(ds.key(1))
        self.assertEquals(before_ts_c.search(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(1))
        before_ts_s.commit_transaction()
        # As read is between(i.e before commit), prev should get old value.
        self.assertEquals(between_ts_c.prev(), 0)
        self.assertEquals(between_ts_c.get_key(), ds.key(1))
        between_ts_s.commit_transaction()
        # As read is after, prev should get new value.
        self.assertEquals(after_ts_c.prev(), wiredtiger.WT_NOTFOUND)
        after_ts_s.commit_transaction()
        # Non-timestamped read should find new key 1.
        self.assertEquals(cursor.prev(), wiredtiger.WT_NOTFOUND)
        session.commit_transaction()

        # End of Scenario-3.

        # Scenario-4 : Check cursor navigate with remove in prepared transaction.
        # remove keys not in the ends.
        # Begin of Scenario-4.
        # Data set at start has keys {2,3,4 ... 50}
        # Remove key 49 to check next operation.
        # Remove key 3 to check prev operation.
        prep_session.begin_transaction()
        prep_cursor.set_key(ds.key(49))
        prep_cursor.remove()
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(700))

        # Point all cursors to key 48.
        before_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(650))
        before_ts_c.set_key(ds.key(48))
        self.assertEquals(before_ts_c.search(), 0)

        between_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(750))
        between_ts_c.set_key(ds.key(48))
        self.assertEquals(between_ts_c.search(), 0)

        after_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(850))
        after_ts_c.set_key(ds.key(48))
        self.assertEquals(after_ts_c.search(), 0)

        session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(ds.key(48))
        self.assertEquals(cursor.search(), 0)

        # Check the visibility of removed prepared update.

        # As read is before prepare timestamp, next is found with key 49.
        self.assertEquals(before_ts_c.next(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(49))
        # As read is between, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.next())
        # As read is after, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: after_ts_c.next())
        # As read is non-timestamped, next will point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.next())

        # Commit the prepared transaction.
        prep_session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(800))
        prep_session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(800))
        prep_session.commit_transaction()

        # Check to see before cursor still gets the old value.
        before_ts_c.set_key(ds.key(49))
        self.assertEquals(before_ts_c.search(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(49))
        before_ts_s.commit_transaction()
        # As read is between(i.e before commit), next is not found.
        self.assertEquals(between_ts_c.next(), 0)
        self.assertEquals(between_ts_c.get_key(), ds.key(49))
        between_ts_s.commit_transaction()
        # As read is after, next will point beyond end.
        self.assertEquals(after_ts_c.next(), 0)
        self.assertEquals(after_ts_c.get_key(), ds.key(50))
        after_ts_s.commit_transaction()
        # Non-timestamped read should not find a key.
        self.assertEquals(cursor.next(), 0)
        self.assertEquals(cursor.get_key(), ds.key(50))
        session.commit_transaction()

        # Remove key 3 to check prev operation.
        prep_session.begin_transaction()
        prep_cursor.set_key(ds.key(3))
        prep_cursor.remove()
        prep_session.prepare_transaction('prepare_timestamp=' + self.timestamp_str(700))

        # Point all cursors to key 4.
        before_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(650))
        before_ts_c.set_key(ds.key(4))
        self.assertEquals(before_ts_c.search(), 0)

        between_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(750))
        between_ts_c.set_key(ds.key(4))
        self.assertEquals(between_ts_c.search(), 0)

        after_ts_s.begin_transaction('isolation=' + self.isolation + ',read_timestamp=' + self.timestamp_str(850))
        after_ts_c.set_key(ds.key(4))
        self.assertEquals(after_ts_c.search(), 0)

        session.begin_transaction('isolation=' + self.isolation)
        cursor.set_key(ds.key(4))
        self.assertEquals(cursor.search(), 0)

        # Check the visibility of new update of prepared transaction.

        # As read is before prepare timestamp, prev is not found.
        self.assertEquals(before_ts_c.prev(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(3))
        # As read is between, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: between_ts_c.prev())
        # As read is after, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: after_ts_c.prev())
        # As read is non-timestamped, prev should point to prepared update.
        self.assertRaisesException(wiredtiger.WiredTigerError, lambda: cursor.prev())

        # Commit the prepared transaction.
        prep_session.timestamp_transaction('commit_timestamp=' + self.timestamp_str(800))
        prep_session.timestamp_transaction('durable_timestamp=' + self.timestamp_str(800))
        prep_session.commit_transaction()

        # Check to see before cursor still gets the old value.
        before_ts_c.set_key(ds.key(3))
        self.assertEquals(before_ts_c.search(), 0)
        self.assertEquals(before_ts_c.get_key(), ds.key(3))
        before_ts_s.commit_transaction()
        # As read is between(i.e before commit), prev should get old value.
        self.assertEquals(between_ts_c.prev(), 0)
        self.assertEquals(between_ts_c.get_key(), ds.key(3))
        between_ts_s.commit_transaction()
        # As read is after, prev should get new value.
        self.assertEquals(after_ts_c.prev(), 0)
        self.assertEquals(after_ts_c.get_key(), ds.key(2))
        after_ts_s.commit_transaction()
        # Non-timestamped read should find new key 2.
        self.assertEquals(cursor.prev(), 0)
        self.assertEquals(cursor.get_key(), ds.key(2))
        session.commit_transaction()

        # End of Scenario-4.

if __name__ == '__main__':
    wttest.run()
