#!/usr/bin/env python
#
# Public Domain 2014-2018 MongoDB, Inc.
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
# test_timestamp10.py
#   Timestamps: timestamp ordering
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest

def timestamp_str(t):
    return '%x' % t

class test_timestamp10(wttest.WiredTigerTestCase, suite_subprocess):
    conn_config = 'verbose=[timestamp]'
    def test_timestamp_range(self):
        if not wiredtiger.timestamp_build() or not wiredtiger.diagnostic_build():
            self.skipTest('requires a timestamp and diagnostic build')

        base = 'timestamp10'
        uri = 'file:' + base
        # Create a data item at a timestamp
        self.session.create(uri, 'key_format=S,value_format=S')

        # Insert a data item at timestamp 2
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(2))
        c['key'] = 'value2'
        self.session.commit_transaction()
        c.close()

        # Modify the data item at timestamp 1
        #
        # The docs say:
        # The commits to a particular data item must be performed in timestamp
        # order. Again, this is only checked in diagnostic builds and if
        # applications violate this rule, data consistency can be violated.
        #
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(1))
        c['key'] = 'value1'
        msg='on new update is older than'
        with self.expectedStdoutPattern(msg):
            self.session.commit_transaction()
        c.close()

        # Make sure we can successfully add a different key at timestamp 1.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(1))
        c['key1'] = 'value1'
        self.session.commit_transaction()
        c.close()

        #
        # Insert key2 at timestamp 10 and key3 at 15.
        # Then modify both keys in one transaction at timestamp 14.
        # Modifying the one from 15 should report a warning message, but
        # the update will be applied.
        #
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(10))
        c['key2'] = 'value10'
        self.session.commit_transaction()
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(15))
        c['key3'] = 'value15'
        self.session.commit_transaction()

        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(14))
        c['key2'] = 'value14'
        c['key3'] = 'value14'
        with self.expectedStdoutPattern(msg):
            self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.assertEquals(c['key2'], 'value14')
        self.assertEquals(c['key3'], 'value14')
        c.close()

        #
        # Separately, we should be able to update key2 at timestamp 16.
        #
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(16))
        c['key2'] = 'value16'
        self.session.commit_transaction()

        # Updating key3 inserted at timestamp 13 will report a warning.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(13))
        c['key3'] = 'value13'
        with self.expectedStdoutPattern(msg):
            self.session.commit_transaction()
        c.close()

        # Test that updating again with an invalid timestamp reports a warning.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(12))
        c['key3'] = 'value12'
        with self.expectedStdoutPattern(msg):
            self.session.commit_transaction()
        c.close()

        c = self.session.open_cursor(uri)
        self.assertEquals(c['key3'], 'value12')
        c.close()

        # Now try a later timestamp.
        c = self.session.open_cursor(uri)
        self.session.begin_transaction()
        self.session.timestamp_transaction(
            'commit_timestamp=' + timestamp_str(17))
        c['key3'] = 'value17'
        self.session.commit_transaction()
        c.close()

if __name__ == '__main__':
    wttest.run()
