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
# test_cursor22.py
#   Test cursor get_raw_key_value

import wttest

class test_cursor22(wttest.WiredTigerTestCase):
    uri = "table:test_cursor22"

    def check_get_key_and_value(self, cursor, expected_key, expected_value):
        key = cursor.get_key()
        value = cursor.get_value()
        self.assertEquals(key, expected_key)
        self.assertEquals(value, expected_value)

    def check_get_raw_key_value(self, cursor, expected_key, expected_value):
        (key, value) = cursor.get_raw_key_value()
        self.assertEquals(key, expected_key)
        self.assertEquals(value, expected_value)

    def test_cursor22(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        cursor = self.session.open_cursor(self.uri)

        # Insert some data
        self.session.begin_transaction()
        for i in range(1, 10):
            cursor.set_key("key" + str(i))
            cursor.set_value("value" + str(100+i))
            cursor.insert()
        self.session.commit_transaction()

        # Check the data using get_key() and get_value()
        self.session.begin_transaction()
        cursor.reset()
        for i in range(1, 10):
            cursor.next()
            self.check_get_key_and_value(cursor=cursor, expected_key="key" + str(i), expected_value="value" + str(100+i))
        self.session.commit_transaction()

        # Check the data using get_raw_key_value()
        self.session.begin_transaction()
        cursor.reset()
        for i in range(1, 10):
            cursor.next()
            self.check_get_raw_key_value(cursor=cursor, expected_key="key" + str(i), expected_value="value" + str(100+i))
        self.session.commit_transaction()

        # Check the less common usage of get_raw_key_value()
        self.session.begin_transaction()
        cursor.reset()
        cursor.next()
        # Get only the key (and ignore the value)
        (key, _) = cursor.get_raw_key_value()
        # Check we can ignore the result completely, without an issue
        cursor.get_raw_key_value()
        # Get only the value (and ignore the key)
        (_, value) = cursor.get_raw_key_value()
        self.assertEquals(key, "key1")
        self.assertEquals(value, "value101")
        self.session.commit_transaction()

        cursor.close()
        self.session.close()


if __name__ == '__main__':
    wttest.run()