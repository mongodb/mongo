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
import wttest
from wtscenario import make_scenarios

# test_empty.py
#       Test that empty objects don't write anything other than a single sector.
class test_empty(wttest.WiredTigerTestCase):
    name = 'test_empty'

    scenarios = make_scenarios([
        ('file-r', dict(type='file:', fmt='r')),
        ('file-S', dict(type='file:', fmt='S')),
        ('table-r', dict(type='table:', fmt='r')),
        ('table-S', dict(type='table:', fmt='S'))
    ])

    # Creating an object and then closing it shouldn't write any blocks.
    def test_empty_create(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=' + self.fmt + ',value_format=S')
        self.session.close()
        name = self.name
        if self.type == "table:":
            name = name + '.wt'
        self.assertEquals(os.stat(name).st_size, 4*1024)

if __name__ == '__main__':
    wttest.run()
