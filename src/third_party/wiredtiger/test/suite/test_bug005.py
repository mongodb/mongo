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
# session_api:verify
# [END_TAGS]
#
# test_bug005.py
#       Regression tests.

import wttest
from wtdataset import simple_key, simple_value

# Check that verify works when the file has additional data after the last
# checkpoint.
class test_bug005(wttest.WiredTigerTestCase):
    # This is a btree layer test, test files, ignore tables.
    uri = 'file:test_bug005'

    def test_bug005(self):
        # Create the object.
        self.session.create(self.uri, 'value_format=S,key_format=S')
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(1, 1000):
            cursor[simple_key(cursor, i)] = simple_value(cursor, i)
        cursor.close()

        # Verify the object, force it to disk, and verify the on-disk version.
        self.verifyUntilSuccess(self.session, self.uri)
        self.reopen_conn()
        self.verifyUntilSuccess(self.session, self.uri)

        # Append random data to the end.
        f = open('test_bug005', 'a')
        f.write('random data')
        f.close()

        # Verify the object again.
        self.verifyUntilSuccess(self.session, self.uri)

if __name__ == '__main__':
    wttest.run()
