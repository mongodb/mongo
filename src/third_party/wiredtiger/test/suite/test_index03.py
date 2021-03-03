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

# test_index03.py
# Make sure cursors cannot stay open while a new index is created.
class test_index03(wttest.WiredTigerTestCase):

    def key(self, i):
        return str('%015d' % i)

    def value(self, i):
        return tuple([str(i+1), str(i+2), str(i+3), str(i+4)])

    def test_index_create(self):
        uri = 'table:test_index03'
        index1_uri = 'index:test_index03:indx1'
        index2_uri = 'index:test_index03:indx2'
        config = ',key_format=S,value_format=SSSS'

        session = self.session
        session.create(uri, 'columns=(key,col1,col2,col3,col4)' + config)
        session.create(index1_uri, 'columns=(col1)' + config)
        c1 = session.open_cursor(uri, None)

        # Having cursors open across index creates is not currently allowed.
        with self.expectedStderrPattern("Can't create an index for table"):
            self.assertRaises(wiredtiger.WiredTigerError,
                lambda: session.create(index2_uri, 'columns=(col2)' + config))
        c1.close()

        session.create(index2_uri, 'columns=(col2)' + config)
        c1 = session.open_cursor(uri, None)
        # Having cursors open across drops is not currently allowed.
        # On the drop side, we need to begin using the cursor
        for i in range(100, 200):
            c1[self.key(i)] = self.value(i)

        self.assertRaises(wiredtiger.WiredTigerError,
            lambda: session.drop(index2_uri))
        c1.close()
        session.drop(index2_uri)

if __name__ == '__main__':
    wttest.run()
