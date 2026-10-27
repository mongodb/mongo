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

import wttest

# test_verify6.py
#    Verify accepts a NULL URI to verify all tables in the database.
class test_verify6(wttest.WiredTigerTestCase):
    uri = 'table:test_verify6'

    def test_verify_null_uri(self):
        params = 'key_format=S,value_format=S'

        # A NULL URI on an empty database has nothing to verify.
        self.assertEqual(self.session.verify(None), 0)

        # Verify all tables through a single NULL URI call.
        for i in range(3):
            uri = f'{self.uri}_{i}'
            self.session.create(uri, params)
            cursor = self.session.open_cursor(uri)
            cursor['key'] = 'value'
            cursor.close()
        self.session.checkpoint()
        self.assertEqual(self.session.verify(None), 0)

        # Verifying a single table still works alongside the NULL URI form.
        self.assertEqual(self.session.verify(f'{self.uri}_0'), 0)
