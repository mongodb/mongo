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

import itertools, wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wtscenario import multiply_scenarios, number_scenarios
from wiredtiger import stat
from helper import complex_populate, complex_populate_lsm, simple_populate
from helper import complex_value_populate, key_populate, value_populate

# test_stat05.py
#    Statistics cursor using size only
class test_stat_cursor_config(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_size'
    conn_config = 'statistics=(fast)'

    uri = [
        ('file',  dict(uri='file:' + pfx, pop=simple_populate, cfg='')),
        ('table', dict(uri='table:' + pfx, pop=simple_populate, cfg='')),
        ('inmem', dict(uri='table:' + pfx, pop=simple_populate, cfg='',
            conn_config='in_memory,statistics=(fast)')),
        ('table-lsm', dict(uri='table:' + pfx, pop=simple_populate,
            cfg=',type=lsm,lsm=(chunk_size=1MB,merge_min=2)')),
        ('complex', dict(uri='table:' + pfx, pop=complex_populate, cfg='')),
        ('complex-lsm',
            dict(uri='table:' + pfx, pop=complex_populate_lsm,
            cfg=',lsm=(chunk_size=1MB,merge_min=2)')),
    ]

    scenarios = number_scenarios(uri)

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
        self.pop(self, self.uri, 'key_format=S' + self.cfg, 100)
        self.openAndWalkStatCursor()
        cursor = self.session.open_cursor(self.uri, None)
        for i in range(100, 40000 + 1):
            if i % 100 == 0:
                self.openAndWalkStatCursor()
            if self.pop == simple_populate:
                cursor[key_populate(cursor, i)] = value_populate(cursor, i)
            else:
                cursor[key_populate(cursor, i)] = \
                        tuple(complex_value_populate(cursor, i))
        cursor.close()
        self.openAndWalkStatCursor()

if __name__ == '__main__':
    wttest.run()
