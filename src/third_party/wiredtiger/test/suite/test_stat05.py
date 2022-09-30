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

import wttest
from wtscenario import make_scenarios
from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet

# test_stat05.py
#    Statistics cursor using size only
class test_stat_cursor_config(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_size'
    conn_config = 'statistics=(fast)'

    uri = [
        ('file-row',  dict(uri='file:' + pfx, dataset=SimpleDataSet, type='row', cfg='')),
        ('file-var',  dict(uri='file:' + pfx, dataset=SimpleDataSet, type='var', cfg='')),
        ('file-fix',  dict(uri='file:' + pfx, dataset=SimpleDataSet, type='fix', cfg='')),
        ('table-row', dict(uri='table:' + pfx, dataset=SimpleDataSet, type='row', cfg='')),
        ('table-var', dict(uri='table:' + pfx, dataset=SimpleDataSet, type='var', cfg='')),
        ('table-fix', dict(uri='table:' + pfx, dataset=SimpleDataSet, type='fix', cfg='')),
        ('inmem-row', dict(uri='table:' + pfx, dataset=SimpleDataSet, type='row', cfg='',
            conn_config = 'in_memory,statistics=(fast)')),
        ('inmem-var', dict(uri='table:' + pfx, dataset=SimpleDataSet, type='var', cfg='',
            conn_config = 'in_memory,statistics=(fast)')),
        ('inmem-fix', dict(uri='table:' + pfx, dataset=SimpleDataSet, type='fix', cfg='',
            conn_config = 'in_memory,statistics=(fast)')),
        ('table-lsm', dict(uri='table:' + pfx, dataset=SimpleDataSet, type='lsm',
            cfg='lsm=(chunk_size=1MB,merge_min=2)',
            conn_config = 'statistics=(fast)')),
        ('complex-row', dict(uri='table:' + pfx, dataset=ComplexDataSet, type='row', cfg='')),
        ('complex-var', dict(uri='table:' + pfx, dataset=ComplexDataSet, type='fix', cfg='')),
        ('complex-lsm',
            dict(uri='table:' + pfx, dataset=ComplexLSMDataSet, type='lsm',
            cfg='lsm=(chunk_size=1MB,merge_min=2)',
            conn_config = 'statistics=(fast)')),
    ]

    scenarios = make_scenarios(uri)

    def openAndWalkStatCursor(self):
        c = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(size)')
        count = 0
        while c.next() == 0:
            count += 1
        c.close()

    # Open a size-only statistics cursor on various table types. Ensure that
    # the cursor open succeeds. Insert enough data that LSM tables to need to
    # switch and merge.
    def test_stat_cursor_size(self):
        if self.type == 'row':
            key_format = 'S'
            value_format = 'S'
        elif self.type == 'var':
            key_format = 'r'
            value_format = 'S'
        elif self.type == 'fix':
            key_format = 'r'
            value_format = '8t'
        else:
            self.assertEqual(self.type, 'lsm')
            key_format = 'S'
            value_format = 'S'

        ds = self.dataset(
           self, self.uri, 100, key_format=key_format, value_format=value_format, config=self.cfg)
        ds.populate()
        self.openAndWalkStatCursor()
        cursor = ds.open_cursor(self.uri, None)
        for i in range(100, 40000 + 1):
            if i % 100 == 0:
                self.openAndWalkStatCursor()
            cursor[ds.key(i)] = ds.value(i)
        cursor.close()
        self.openAndWalkStatCursor()

if __name__ == '__main__':
    wttest.run()
