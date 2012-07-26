#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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
import wiredtiger, wttest
from helper import keyPopulate

# test_empty.py
#       Test that empty objects don't write anything other than a single sector.
class test_empty(wttest.WiredTigerTestCase):
    name = 'test_empty'

    scenarios = [
        ('file', dict(type='file:', fmt='r')),
        ('file', dict(type='file:', fmt='S')),
        ('table', dict(type='table:', fmt='r')),
        ('table', dict(type='table:', fmt='S'))
        ]

    # Creating an object and then closing it shouldn't write any blocks.
    def test_empty_create(self):
        uri = self.type + self.name
        self.session.create(uri, 'key_format=' + self.fmt)
        self.session.close();
        name = self.name
        if self.type == "table:":
            name = name + '.wt'
        self.assertEquals(os.stat(name).st_size, 512)

    # Creating an object, inserting a record and then removing it (that is,
    # building an empty, dirty tree), shouldn't write any blocks.  This is
    # not true for column-store objects, though, even deleting an object
    # modifies the name space, which requires a write.
    def test_empty(self):
        if self.fmt == 'S':
            uri = self.type + self.name
            self.session.create(uri, 'key_format=' + self.fmt)

            # Add a few records and remove them.
            cursor = self.session.open_cursor(uri, None, None)
            for i in range(1,5):
                cursor.set_key(keyPopulate(self.fmt, i));
                cursor.set_value("XXX");
                cursor.insert();
                cursor.remove();
            self.session.close();

            name = self.name
            if self.type == "table:":
                name = name + '.wt'
            self.assertEquals(os.stat(name).st_size, 512)

if __name__ == '__main__':
    wttest.run()
