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

import wiredtiger, wttest
from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# test_cursor09.py
#    JIRA WT-2217: insert resets key/value "set".
class test_cursor09(wttest.WiredTigerTestCase):
    scenarios = make_scenarios([
        ('file-f', dict(type='file:', keyfmt='r', valfmt='8t', dataset=SimpleDataSet)),
        ('file-r', dict(type='file:', keyfmt='r', valfmt='S', dataset=SimpleDataSet)),
        ('file-S', dict(type='file:', keyfmt='S', valfmt='S', dataset=SimpleDataSet)),
        ('lsm-S', dict(type='lsm:', keyfmt='S', valfmt='S', dataset=SimpleDataSet)),
        ('table-f', dict(type='table:', keyfmt='r', valfmt='8t', dataset=SimpleDataSet)),
        ('table-r', dict(type='table:', keyfmt='r', valfmt='S', dataset=SimpleDataSet)),
        ('table-S', dict(type='table:', keyfmt='S', valfmt='S', dataset=SimpleDataSet)),
        ('table-r-complex', dict(type='table:', keyfmt='r', valfmt=None,
            dataset=ComplexDataSet)),
        ('table-S-complex', dict(type='table:', keyfmt='S', valfmt=None,
            dataset=ComplexDataSet)),
        ('table-S-complex-lsm', dict(type='table:', keyfmt='S', valfmt=None,
            dataset=ComplexLSMDataSet)),
    ])

    # WT_CURSOR.insert doesn't leave the cursor positioned, verify any
    # subsequent cursor operation fails with a "key not set" message.
    def test_cursor09(self):
        uri = self.type + 'cursor09'

        ds = self.dataset(self, uri, 100, key_format=self.keyfmt, value_format=self.valfmt)
        ds.populate()

        cursor = ds.open_cursor(uri, None, None)
        cursor[ds.key(10)] = ds.value(10)
        msg = '/requires key be set/'
        self.assertRaisesWithMessage(
            wiredtiger.WiredTigerError, cursor.search, msg)

if __name__ == '__main__':
    wttest.run()
