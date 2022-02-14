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

# test_join03.py
#    Join operations
# Joins with a custom extractor
class test_join03(wttest.WiredTigerTestCase):
    table_name1 = 'test_join03'
    nentries = 100

    def conn_extensions(self, extlist):
        extlist.skip_if_missing = True
        extlist.extension('extractors', 'csv')

    def gen_key(self, i):
        return [ i + 1 ]

    def gen_values(self, i):
        s = str(i)
        rs = s[::-1].lstrip('0')
        return [ s + ',' + rs ]

    # Common function for testing iteration of join cursors
    def iter_common(self, jc):
        mbr = set([62, 63, 72, 73, 82, 83, 92, 93])
        while jc.next() == 0:
            [k] = jc.get_keys()
            i = k - 1
            [v] = jc.get_values()
            self.assertEquals(self.gen_values(i), [v])
            if not i in mbr:
                self.tty('  result ' + str(i) + ' is not in: ' + str(mbr))
            self.assertTrue(i in mbr)
            mbr.remove(i)
        self.assertEquals(0, len(mbr))

    # Common function for testing the most basic functionality
    # of joins
    def join(self, csvformat, args0, args1):
        self.session.create('table:join03', 'key_format=r' +
                            ',value_format=S,columns=(k,v)')
        fmt = csvformat[0]
        self.session.create('index:join03:index0','key_format=' + fmt + ',' +
                            'extractor=csv,app_metadata={"format" : "' +
                            fmt + '","field" : "0"}')
        fmt = csvformat[1]
        self.session.create('index:join03:index1','key_format=' + fmt + ',' +
                            'extractor=csv,app_metadata={"format" : "' +
                            fmt + '","field" : "1"}')

        c = self.session.open_cursor('table:join03', None, None)
        for i in range(0, self.nentries):
            c.set_key(*self.gen_key(i))
            c.set_value(*self.gen_values(i))
            c.insert()
        c.close()

        jc = self.session.open_cursor('join:table:join03', None, None)

        # All the numbers 0-99 whose string representation
        # sort >= '60' and whose reverse string representation
        # is in '20' < x < '40'.  That is: [62, 63, 72, 73, 82, 83, 92, 93]
        c0 = self.session.open_cursor('index:join03:index0', None, None)
        if csvformat[0] == 'S':
            c0.set_key('60')
        else:
            c0.set_key(60)
        self.assertEquals(0, c0.search())
        self.session.join(jc, c0, 'compare=ge' + args0)

        c1a = self.session.open_cursor('index:join03:index1', None, None)
        if csvformat[1] == 'S':
            c1a.set_key('21')
        else:
            c1a.set_key(21)
        self.assertEquals(0, c1a.search())
        self.session.join(jc, c1a, 'compare=gt' + args1)

        c1b = self.session.open_cursor('index:join03:index1', None, None)
        if csvformat[1] == 'S':
            c1b.set_key('41')
        else:
            c1b.set_key(41)
        self.assertEquals(0, c1b.search())
        self.session.join(jc, c1b, 'compare=lt' + args1)

        # Iterate, and make sure that reset allows us to iterate again.
        self.iter_common(jc)

        jc.close()
        c1a.close()
        c1b.close()
        c0.close()
        self.session.drop('table:join03')

    # Test joins using CSV fields that are interpreted as different types
    # to make sure all the extractor plumbing used in joins is working.
    def test_join(self):
        for extraargs in [ '', ',strategy=bloom,count=1000' ]:
            for csvformat in [ 'SS', 'ii', 'Si', 'iS' ]:
                self.join(csvformat, '', extraargs)

if __name__ == '__main__':
    wttest.run()
