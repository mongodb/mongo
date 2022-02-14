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

import os
from suite_subprocess import suite_subprocess
import wttest

# test_util15.py
#    Utilities: wt alter
class test_util15(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util15.a'

    def test_alter_process(self):
        """
        Test alter in a 'wt' process
        """
        params = 'key_format=S,value_format=S'
        self.session.create('table:' + self.tablename, params)
        self.assertTrue(os.path.exists(self.tablename + ".wt"))

        """
        Alter access pattern and confirm
        """
        acc_pat_seq="access_pattern_hint=sequential"
        self.runWt(["alter", "table:" + self.tablename, acc_pat_seq])
        cursor = self.session.open_cursor("metadata:create", None, None)
        cursor.set_key("table:" + self.tablename)
        self.assertEqual(cursor.search(),0)
        string = cursor.get_value()
        cursor.close()
        self.assertTrue(acc_pat_seq in string)

        """
        Alter access pattern again and confirm
        """
        acc_pat_rand="access_pattern_hint=random"
        self.runWt(["alter", "table:" + self.tablename, acc_pat_rand])
        cursor = self.session.open_cursor("metadata:create", None, None)
        cursor.set_key("table:" + self.tablename)
        self.assertEqual(cursor.search(),0)
        string = cursor.get_value()
        cursor.close()
        self.assertTrue(acc_pat_rand in string)

if __name__ == '__main__':
    wttest.run()
