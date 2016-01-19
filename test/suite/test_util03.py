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

from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import check_scenarios

# test_util03.py
#    Utilities: wt create
class test_util03(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util03.a'
    nentries = 1000

    scenarios = check_scenarios([
        ('none', dict(key_format=None,value_format=None)),
        ('SS', dict(key_format='S',value_format='S')),
        ('rS', dict(key_format='r',value_format='S')),
        ('ri', dict(key_format='r',value_format='i')),
    ])

    def test_create_process(self):
        """
        Test create in a 'wt' process
        """

        args = ["create"]
        if self.key_format != None or self.value_format != None:
            args.append('-c')
            config = ''
            if self.key_format != None:
                config += 'key_format=' + self.key_format + ','
            if self.value_format != None:
                config += 'value_format=' + self.value_format
            args.append(config)
        args.append('table:' + self.tablename)
        self.runWt(args)

        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        if self.key_format != None:
            self.assertEqual(cursor.key_format, self.key_format)
        if self.value_format != None:
            self.assertEqual(cursor.value_format, self.value_format)
        for key,val in cursor:
            self.fail('table should be empty')
        cursor.close()


if __name__ == '__main__':
    wttest.run()
