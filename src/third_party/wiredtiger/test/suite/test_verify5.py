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

import wiredtiger, wttest

# test_verify5.py
#    Verify must report a dump_offsets block that cannot be read.
class test_verify5(wttest.WiredTigerTestCase):
    test_name = __qualname__
    uri = f'file:{test_name}.wt'

    def test_dump_offsets_read_error(self):
        if not wiredtiger.diagnostic_build():
            self.skipTest('dump_offsets requires a diagnostic build')

        self.session.create(self.uri, 'key_format=i,value_format=S')
        cursor = self.session.open_cursor(self.uri)
        for i in range(100):
            cursor[i] = 'value'
        cursor.close()
        self.session.checkpoint()

        # An offset well past end of file cannot be read, and the failure has to reach the caller.
        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: self.session.verify(self.uri, 'dump_offsets=[1048576]'))
        self.ignoreStderrPatternIfExists('failed to read')


if __name__ == '__main__':
    wttest.run()
