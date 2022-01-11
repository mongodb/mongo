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
from wtdataset import SimpleDataSet

# test_prepare18.py
#    Test that prepare on a logged file returns an error.
class test_prepare18(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled)'

    def test_prepare18(self):
        uri = "table:prepare18"
        ds = SimpleDataSet(self, uri, 100, key_format='S', value_format='S')
        ds.populate()
        cursor = self.session.open_cursor(uri, None)
        self.session.begin_transaction()
        cursor[ds.key(10)] = ds.value(20)
        self.session.commit_transaction()
        self.session.begin_transaction()
        cursor[ds.key(10)] = ds.value(20)
        msg='/a prepared transaction cannot include a logged table/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda:self.session.prepare_transaction('prepare_timestamp=1'), msg)

if __name__ == '__main__':
    wttest.run()
