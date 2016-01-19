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
from helper import copy_wiredtiger_home, key_populate, simple_populate

# test_bug015.py
#    JIRA WT-2162: index drop in a certain order triggers NULL pointer deref
class test_bug015(wttest.WiredTigerTestCase):
    def test_bug015(self):
        table = 'table:test_bug015'
        idx1 = 'index:test_bug015:aab'
        idx2 = 'index:test_bug015:aaa'
        self.session.create(table, "columns=(k,v)")
        self.session.create(idx1, "columns=(v)")
        self.session.create(idx2, "columns=(v)")
        self.session.drop(idx1, "force=true")
        self.session.create(idx1, "columns=(v)")
        self.session.drop(idx2, "force=true")
        self.session.create(idx2, "columns=(v)")

if __name__ == '__main__':
    wttest.run()
