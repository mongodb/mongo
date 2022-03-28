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
# test_cursor20.py
#   Test duplicate key return values.

from suite_subprocess import suite_subprocess
from wtdataset import SimpleDataSet
import wiredtiger, wttest
from wtscenario import make_scenarios

class test_cursor20(wttest.WiredTigerTestCase, suite_subprocess):
    format_values = [
        ('row', dict(key_format = 'S', value_format='S')),
        ('var', dict(key_format = 'r', value_format='S')),
        ('fix', dict(key_format = 'r', value_format='8t')),
    ]
    reopen = [
        ('in-memory', dict(reopen=False)),
        ('on-disk', dict(reopen=True)),
    ]
    scenarios = make_scenarios(format_values, reopen)

    def test_dup_key(self):
        uri = 'table:dup_key'
        ds = SimpleDataSet(self, uri, 100,
            key_format=self.key_format, value_format=self.value_format)
        ds.populate()

        if self.reopen:
            self.reopen_conn()

        c = self.session.open_cursor(uri, None, 'overwrite=false')
        c.set_key(ds.key(10))
        c.set_value(ds.value(20))
        self.assertRaisesHavingMessage(
            wiredtiger.WiredTigerError, lambda:c.insert(), '/WT_DUPLICATE_KEY/')
        self.assertEqual(c.get_value(), ds.value(10))

if __name__ == '__main__':
    wttest.run()
