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
# THE SOFTWARE IS PROVIDED 'AS IS', WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

import wttest

# test_lsm02.py
#    Test LSM schema level operations
class test_lsm02(wttest.WiredTigerTestCase):
    uri = 'lsm:test_lsm02'

    def add_key(self, uri, key, value):
        cursor = self.session.open_cursor(uri, None, None)
        cursor[key] = value
        cursor.close()

    def verify_key_exists(self, uri, key, value):
        cursor = self.session.open_cursor(uri, None, None)
        cursor.set_key(key)
        cursor.search()
        if value != cursor.get_value():
            print('Unexpected value from LSM tree')
        cursor.close()

    # Put some special values that start with the LSM tombstone
    def test_lsm_tombstone(self):
        self.session.create(self.uri, 'key_format=S,value_format=u')
        v = b'\x14\x14'
        self.add_key(self.uri, 'k1', v)
        self.verify_key_exists(self.uri, 'k1', v)
        v = b'\x14\x14\0\0\0\0\0\0'
        self.add_key(self.uri, 'k2', v)
        self.verify_key_exists(self.uri, 'k2', v)
        v += b'a' * 1000
        self.add_key(self.uri, 'k3', v)
        self.verify_key_exists(self.uri, 'k3', v)

    def test_lsm_rename01(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.add_key(self.uri, 'a', 'a')
        self.renameUntilSuccess(self.session, self.uri, self.uri + 'renamed')
        self.verify_key_exists(self.uri + 'renamed', 'a', 'a')

    def test_lsm_rename02(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')
        self.add_key(self.uri, 'a', 'a')
        self.renameUntilSuccess(self.session, self.uri, self.uri + 'renamed')

        # Create a new LSM with the original name
        self.session.create(self.uri, 'key_format=S,value_format=S')

        # Add a different entry to the new tree
        self.add_key(self.uri, 'b', 'b')

        self.verify_key_exists(self.uri + 'renamed', 'a', 'a')
        self.verify_key_exists(self.uri, 'b', 'b')

if __name__ == '__main__':
    wttest.run()
