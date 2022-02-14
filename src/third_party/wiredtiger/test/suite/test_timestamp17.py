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
# test_timestamp17.py
#   Test unintended timestamp usage on an update and ensure behavior
#   matches expectations. Additionally, move the timestamp to ensure
#   that values read are still consistent after those timestamps are
#   moved.
#

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_timestamp17(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_timestamp17'
    uri = 'table:' + tablename

    format_values = [
        ('integer-row', dict(key_format='i', value_format='i')),
        ('column', dict(key_format='r', value_format='i')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    scenarios = make_scenarios(format_values)

    def test_inconsistent_timestamping(self):
        format = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(self.uri, format)
        self.session.begin_transaction()
        cur1 = self.session.open_cursor(self.uri)
        cur1[1] = 1
        self.session.commit_transaction('commit_timestamp=25')

        self.session.begin_transaction()
        cur1[1] = 2
        self.session.commit_transaction('commit_timestamp=50')

        self.session.begin_transaction()
        cur1[1] = 3
        self.session.commit_transaction('commit_timestamp=200')

        self.session.begin_transaction()
        cur1.set_key(1)
        cur1.remove()
        self.session.commit_transaction('commit_timestamp=100')

        # Read before any updates and ensure we cannot find the key or value.
        # (For FLCS we expect to read zeros since the table extends nontransactionally.)
        self.session.begin_transaction('read_timestamp=20')
        cur1.set_key(1)
        search_success = cur1.search()
        if self.value_format == '8t':
            self.assertEqual(search_success, 0)
            self.assertEqual(cur1.get_value(), 0)
        else:
            self.assertEqual(search_success, wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

        # Read at 25 and we should see 1.
        self.session.begin_transaction('read_timestamp=25')
        cur1.set_key(1)
        search_success = cur1.search()
        self.assertEqual(search_success, 0)
        value1 = cur1.get_value()
        self.session.commit_transaction()
        self.assertEqual(1, value1)

        # Read at 50 and we should see 2.
        self.session.begin_transaction('read_timestamp=50')
        cur1.set_key(1)
        search_success = cur1.search()
        self.assertEqual(search_success, 0)
        value1 = cur1.get_value()
        self.session.commit_transaction()
        self.assertEqual(2, value1)

        # Read at 100 and we should not find anything.
        # (For FLCS, deleted values read as zero.)
        self.session.begin_transaction('read_timestamp=100')
        cur1.set_key(1)
        search_success = cur1.search()
        if self.value_format == '8t':
            self.assertEqual(search_success, 0)
            self.assertEqual(cur1.get_value(), 0)
        else:
            self.assertEqual(search_success, wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

        # Read at 200 and we should still not find anything.
        self.session.begin_transaction('read_timestamp=200')
        cur1.set_key(1)
        search_success = cur1.search()
        if self.value_format == '8t':
            self.assertEqual(search_success, 0)
            self.assertEqual(cur1.get_value(), 0)
        else:
            self.assertEqual(search_success, wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

        # Read at 300 for further validation.
        self.session.begin_transaction('read_timestamp=300')
        cur1.set_key(1)
        search_success = cur1.search()
        if self.value_format == '8t':
            self.assertEqual(search_success, 0)
            self.assertEqual(cur1.get_value(), 0)
        else:
            self.assertEqual(search_success, wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

        # Move oldest timestamp forward and
        # confirm we see the correct numbers.
        self.conn.set_timestamp('oldest_timestamp=49')

        # Read at 49 and we should see 1.
        self.session.begin_transaction('read_timestamp=49')
        cur1.set_key(1)
        search_success = cur1.search()
        self.assertEqual(search_success, 0)
        value1 = cur1.get_value()
        self.session.commit_transaction()
        self.assertEqual(1, value1)

        self.conn.set_timestamp('oldest_timestamp=99')

        # Read at 99 and we should see 2.
        self.session.begin_transaction('read_timestamp=99')
        cur1.set_key(1)
        search_success = cur1.search()
        self.assertEqual(search_success, 0)
        value1 = cur1.get_value()
        self.session.commit_transaction()
        self.assertEqual(2, value1)

        # Move oldest to the point at which we deleted.
        self.conn.set_timestamp('oldest_timestamp=100')

        # Read at 100 and we should not find anything.
        # (Again, in FLCS deleted values read back as 0.)
        self.session.begin_transaction('read_timestamp=100')
        cur1.set_key(1)
        search_success = cur1.search()
        if self.value_format == '8t':
            self.assertEqual(search_success, 0)
            self.assertEqual(cur1.get_value(), 0)
        else:
            self.assertEqual(search_success, wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

        # Read at 200 and we should not find anything.
        self.session.begin_transaction('read_timestamp=200')
        cur1.set_key(1)
        search_success = cur1.search()
        if self.value_format == '8t':
            self.assertEqual(search_success, 0)
            self.assertEqual(cur1.get_value(), 0)
        else:
            self.assertEqual(search_success, wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

        # Move oldest timestamp to 200 to ensure history
        # works as expected and we do not see the value 3.
        self.conn.set_timestamp('oldest_timestamp=200')

        self.session.begin_transaction('read_timestamp=200')
        cur1.set_key(1)
        search_success = cur1.search()
        if self.value_format == '8t':
            self.assertEqual(search_success, 0)
            self.assertEqual(cur1.get_value(), 0)
        else:
            self.assertEqual(search_success, wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

        self.session.begin_transaction('read_timestamp=250')
        cur1.set_key(1)
        search_success = cur1.search()
        if self.value_format == '8t':
            self.assertEqual(search_success, 0)
            self.assertEqual(cur1.get_value(), 0)
        else:
            self.assertEqual(search_success, wiredtiger.WT_NOTFOUND)
        self.session.commit_transaction()

if __name__ == '__main__':
    wttest.run()
