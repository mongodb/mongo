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
from wiredtiger import WiredTigerError, wiredtiger_strerror, WT_ROLLBACK, WT_PREPARE_CONFLICT
from wtdataset import SimpleDataSet
from wtscenario import make_scenarios

# test_flcs06.py
# Test FLCS slow-truncate behavior on already-deleted values. In general deleting
# an already-deleted value is an error, but in FLCS because deleted values read
# back as zero (and zero values are deleted) this works differently: you can always
# delete a zero value again. There is special-case code in both remove and truncate
# to take care of this. This test checks truncate.
#
# The behavior of truncate is that it deletes any value that it can see. In FLCS
# this includes zero values. The special-case code in truncate avoids posting a
# tombstone on top of a zero value (to avoid violating the internal restriction
# against consecutive tombstones) but still needs to make a conflict check there,
# in case there's an update on top of the zero that it can't see. Said conflict
# check was missing at one point; this test is to make sure that doesn't happen
# again.

class test_flcs06(wttest.WiredTigerTestCase):
    session_config = 'isolation=snapshot'
    conn_config = 'log=(enabled=false)'

    # Hook to run using remove instead of truncate for reference. This should not alter the
    # behavior... but may if things are broken. Disable the reference version by default as it's
    # only useful when investigating behavior changes. This list is first in the make_scenarios
    # call so the additional cases don't change the scenario numbering.
    trunc_values = [
        ('truncate', dict(trunc_with_remove=False)),
        #('remove', dict(trunc_with_remove=True)),
    ]
    zero_values = [
        ('zero', dict(do_zero=True)),
        ('trunc', dict(do_zero=False)),
    ]
    evict_values = [
        ('no-evict', dict(do_evict=False)),
        ('evict', dict(do_evict=True)),
    ]
    prepare_values = [
        ('no-prepare', dict(do_prepare=False)),
        ('prepare', dict(do_prepare=True)),
    ]
    scenarios = make_scenarios(trunc_values, zero_values, evict_values, prepare_values)

    # This test is FLCS-specific, so run it only on FLCS.
    key_format = 'r'
    value_format = '8t'
    extraconfig = ',allocation_size=512,leaf_page_max=512'

    def truncate(self, uri, key1, key2):
        if self.trunc_with_remove:
            # Because remove clears the cursor position, removing by cursor-next is a nuisance.
            scan_cursor = self.session.open_cursor(uri)
            del_cursor = self.session.open_cursor(uri)
            err = 0
            scan_cursor.set_key(key1)
            self.assertEqual(scan_cursor.search(), 0)
            while scan_cursor.get_key() <= key2:
                del_cursor.set_key(scan_cursor.get_key())
                try:
                    err = del_cursor.remove()
                except WiredTigerError as e:
                    if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                        err = WT_ROLLBACK
                    elif wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                        err = WT_PREPARE_CONFLICT
                    else:
                        raise e
                if err != 0:
                    break
                if scan_cursor.get_key() == key2:
                    break
                try:
                    err = scan_cursor.next()
                except WiredTigerError as e:
                    if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                        err = WT_ROLLBACK
                    elif wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                        err = WT_PREPARE_CONFLICT
                    else:
                        raise e
                if err != 0:
                    break
            scan_cursor.close()
            del_cursor.close()
        else:
            lo_cursor = self.session.open_cursor(uri)
            hi_cursor = self.session.open_cursor(uri)
            lo_cursor.set_key(key1)
            hi_cursor.set_key(key2)
            try:
                err = self.session.truncate(None, lo_cursor, hi_cursor, None)
            except WiredTigerError as e:
                if wiredtiger_strerror(WT_ROLLBACK) in str(e):
                    err = WT_ROLLBACK
                elif wiredtiger_strerror(WT_PREPARE_CONFLICT) in str(e):
                    err = WT_PREPARE_CONFLICT
                else:
                    raise e
            lo_cursor.close()
            hi_cursor.close()
        return err

    def evict(self, uri, lo, hi, value):
        evict_cursor = self.session.open_cursor(uri, None, "debug=(release_evict)")
        self.session.begin_transaction()

        # Evict every 53rd key to make sure we get all the pages but not write them out
        # over and over again any more than necessary. FUTURE: improve this to evict
        # each page once when we get a suitable interface for that.
        for i in range(lo, hi, 53):
            evict_cursor.set_key(i)
            self.assertEquals(evict_cursor.search(), 0)
            self.assertEquals(evict_cursor.get_value(), value)
            evict_cursor.reset()
        self.session.rollback_transaction()
        evict_cursor.close()

    def test_flcs(self):
        nrows = 5000

        # Create a table without logging.
        uri = "table:flcs06"
        ds = SimpleDataSet(
            self, uri, 0, key_format=self.key_format, value_format=self.value_format,
            config='log=(enabled=false)' + self.extraconfig)
        ds.populate()

        value_a = 97
        value_b = 98

        # We want two sessions.
        session1 = self.session
        session2 = self.conn.open_session()

        cursor1 = session1.open_cursor(uri)
        cursor2 = session2.open_cursor(uri)

        # Write out some baseline data.
        session1.begin_transaction()
        for i in range (1, nrows + 1):
            cursor1[i] = value_a
        session1.commit_transaction()

        # Delete it.
        session1.begin_transaction()
        if self.do_zero:
            for i in range (1, nrows + 1):
                cursor1[i] = 0
        else:
            self.assertEqual(self.truncate(uri, 1, nrows), 0)
        session1.commit_transaction()

        # Optionally evict it.
        if self.do_evict:
            self.evict(uri, 1, nrows + 1, 0)

        # Modify some of the data in session2 but don't commit. Optionally prepare.
        session2.begin_transaction()
        for i in range (nrows // 2 + 1, nrows + 1):
            cursor2[i] = value_b
        if self.do_prepare:
            session2.prepare_transaction('prepare_timestamp=' + self.timestamp_str(20))

        # Truncate all of it. (This uses session1.) This should generate a conflict.

        # Expect WT_PREPARE_CONFLICT if session2 prepared; otherwise WT_ROLLBACK.
        expected = WT_PREPARE_CONFLICT if self.do_prepare else WT_ROLLBACK

        # Except: it seems that truncate produces WT_ROLLBACK regardless. Not sure if this
        # is a bug. For now, enforce the current behavior so that it doesn't change by
        # accident.
        if self.trunc_with_remove == False and expected == WT_PREPARE_CONFLICT:
            expected = WT_ROLLBACK

        # Now do it.
        session1.begin_transaction()
        self.assertEqual(self.truncate(uri, 1, nrows), expected)
        session1.rollback_transaction()

        # Tidy up.
        session2.rollback_transaction()
        cursor1.close()
        cursor2.close()
        session2.close()

if __name__ == '__main__':
    wttest.run()
