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

# test_config06.py
#    Test session.create configurations.
class test_config06(wttest.WiredTigerTestCase):
    uri = 'table:test_config06'
    key = 'keyABCDEFGHIJKLMNOPQRSTUVWXYZ'
    value = 'valueABCDEFGHIJKLMNOPQRSTUVWXYZ'

    def session_config(self, config):
        msg = '/Invalid argument/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.create(self.uri, config), msg)

    # Edge cases for key/value formats.
    def test_session_config(self):
        self.session_config('key_format=A,value_format=S')
        self.session_config('key_format=S,value_format=A')
        self.session_config('key_format=0s,value_format=s')
        self.session_config('key_format=s,value_format=0s')
        self.session_config('key_format=0t,value_format=4t')
        self.session_config('key_format=4t,value_format=0t')

    # Smoke-test the string formats with length specifiers; both formats should
    # ignore trailing bytes, verify that.
    def format_string(self, fmt, len):
        k = self.key
        v = self.value
        self.session.create(self.uri, \
            "key_format=" + str(len) + fmt + ",value_format=" + str(len) + fmt)
        cursor = self.session.open_cursor(self.uri, None)
        cursor[k] = v
        self.assertEquals(cursor[k[:len]], v[:len])
    def test_format_string_S_1(self):
        self.format_string('S', 1)
    def test_format_string_S_4(self):
        self.format_string('S', 4)
    def test_format_string_S_10(self):
        self.format_string('S', 10)
    def test_format_string_s_1(self):
        self.format_string('s', 1)
    def test_format_string_s_4(self):
        self.format_string('s', 4)
    def test_format_string_s_10(self):
        self.format_string('s', 10)

    def test_format_string_S_default(self):
        k = self.key
        v = self.value
        self.session.create(self.uri, "key_format=S,value_format=S")
        cursor = self.session.open_cursor(self.uri, None)
        cursor[k] = v
        self.assertEquals(cursor[k], v)

    def test_format_string_s_default(self):
        k = self.key
        v = self.value
        self.session.create(self.uri, "key_format=s,value_format=s")
        cursor = self.session.open_cursor(self.uri, None)
        cursor[k] = v
        self.assertEquals(cursor[k[:1]], v[:1])


if __name__ == '__main__':
    wttest.run()
