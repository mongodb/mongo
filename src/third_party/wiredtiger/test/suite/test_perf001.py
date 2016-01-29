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
#
# test_perf001.py
#       Test performance when inserting into a table with an index.

import wiredtiger, wttest
import random
from time import clock, time
from wtscenario import check_scenarios

# Test performance of inserting into a table with an index.
class test_perf001(wttest.WiredTigerTestCase):
    table_name = 'test_perf001'

    scenarios = check_scenarios([
        #('file-file', dict(tabletype='file',indextype='file')),
        ('file-lsm', dict(tabletype='file',indextype='lsm')),
        #('lsm-file', dict(tabletype='lsm',indextype='file')),
        #('lsm-lsm', dict(tabletype='lsm',indextype='lsm')),
    ])
    conn_config = 'cache_size=512M'

    def test_performance_of_indices(self):
        uri = 'table:' + self.table_name
        create_args = 'key_format=i,value_format=ii,columns=(a,c,d),type=' + self.tabletype
        self.session.create(uri, create_args)
        self.session.create('index:' + self.table_name + ':ia',
            'columns=(d,c),type=' + self.indextype)

        c = self.session.open_cursor('table:' + self.table_name, None, None)
        start_time = clock()
        for i in xrange(750000):
            # 100 operations should never take 5 seconds, sometimes they take
            # 2 seconds when a page is being force-evicted.
            if i % 100 == 0 and i != 0:
                end_time = clock()
                self.assertTrue(end_time - start_time < 5)
                start_time = end_time
            c[i] = (int(time()), random.randint(1,5))
        c.close()

if __name__ == '__main__':
    wttest.run()
