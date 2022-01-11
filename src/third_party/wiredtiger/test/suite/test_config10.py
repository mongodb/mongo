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
# test_config10.py
#   Test valid behaviour when starting WiredTiger with missing or empty
#   WiredTiger version file.

import wiredtiger, wttest
from wiredtiger import stat
import os

class test_config10(wttest.WiredTigerTestCase):
    uri = 'table:config10.'
    
    def test_missing_version_file(self):
        self.conn.close()
        os.remove('WiredTiger')
        # Ensure error occurs when WiredTiger file is missing.
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.setUpConnectionOpen('.'), '/WT_TRY_SALVAGE: database corruption detected/')
    
    def test_empty_version_file(self):
        self.conn.close()
        # Ensure returns message when WiredTiger file is empty.
        open('WiredTiger','w').close()
        expectMessage = 'WiredTiger version file is empty'
        with self.expectedStdoutPattern(expectMessage):
            self.setUpConnectionOpen('.')

    def test_missing_version_file_with_salvage(self):
        self.conn.close()
        os.remove('WiredTiger')
        salvage_config = 'salvage=true'
        self.conn = self.wiredtiger_open('.', salvage_config)
        # Check salvage creates and populates file.
        self.assertNotEqual(os.stat('WiredTiger').st_size, 0)

    def test_empty_version_file_with_salvage(self):
        self.conn.close()
        open('WiredTiger','w').close()
        salvage_config = 'salvage=true'
        self.conn = self.wiredtiger_open('.', salvage_config)
        # Check salvage populates file.
        self.assertNotEqual(os.stat('WiredTiger').st_size, 0)

if __name__ == '__main__':
    wttest.run()
