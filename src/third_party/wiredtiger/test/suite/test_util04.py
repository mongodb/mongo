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

import os
from suite_subprocess import suite_subprocess
import wiredtiger, wttest

# test_util04.py
#    Utilities: wt drop
class test_util04(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util04.a'
    nentries = 1000

    def test_drop_process(self):
        """
        Test drop in a 'wt' process
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)

        self.assertTrue(os.path.exists(self.tablename + ".wt"))
        self.runWt(["drop", "table:" + self.tablename])

        self.assertFalse(os.path.exists(self.tablename + ".wt"))
        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            self.session.open_cursor('table:' + self.tablename, None, None))

if __name__ == '__main__':
    wttest.run()
