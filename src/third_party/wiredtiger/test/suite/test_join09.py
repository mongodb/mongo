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

# test_join09.py
#    Join bloom filters with false positives
class test_join09(wttest.WiredTigerTestCase):
    nentries = 1000

    bloomscen = [
        ('nobloom', dict(false_positives=False, config='')),
        ('bloom1000', dict(false_positives=False,
            config='strategy=bloom,count=1000')),
        ('bloom10fp', dict(false_positives=True,
            config='strategy=bloom,count=10,bloom_false_positives=true'))
    ]

    scenarios = make_scenarios(bloomscen)

    def gen_values(self, i):
        s = str(i)                    # 345 => "345"
        f = s[0:1] + s[0:1] + s[0:1]  # 345 => "333"
        return [s, f]

    def populate(self, s, gen_values):
        c = s.open_cursor('table:join09', None, None)
        for i in range(0, self.nentries):
            c.set_key(i)
            c.set_value(*gen_values(i))
            c.insert()
        c.close()

    # Common function for testing the most basic functionality
    # of joins
    def test_join(self):
        self.session.create('table:join09',
                            'columns=(k,v0,v1),key_format=i,value_format=SS')
        self.session.create('index:join09:index0','columns=(v0)')
        self.session.create('index:join09:index1','columns=(v1)')

        self.populate(self.session, self.gen_values)

        jc = self.session.open_cursor('join:table:join09', None, None)
        c0 = self.session.open_cursor('index:join09:index0', None, None)
        c0.set_key('520')
        self.assertEquals(0, c0.search())
        self.session.join(jc, c0, 'compare=ge')

        joinconfig = 'compare=eq,' + self.config
        c1 = self.session.open_cursor('index:join09:index1', None, None)
        c1.set_key('555')
        self.assertEquals(0, c1.search())
        self.session.join(jc, c1, joinconfig)

        mbr = set(range(520,600)) | set(range(53,60))

        fp_count = 0
        while jc.next() == 0:
            [k] = jc.get_keys()
            [v0,v1] = jc.get_values()
            self.assertEquals(self.gen_values(k), [v0, v1])
            if not k in mbr:
                # With false positives, we can see extra values
                if self.false_positives:
                    fp_count += 1
                    continue
                self.tty('**** ERROR: result ' + str(k) + ' is not in: ' +
                         str(mbr))
            self.assertTrue(k in mbr)
            mbr.remove(k)

        if len(mbr) != 0:
            self.tty('**** ERROR: did not see these: ' + str(mbr))
        self.assertEquals(0, len(mbr))

        # Turning on false positives does not guarantee we'll see extra
        # values, but we've configured our test with a low count to
        # make sure it happens.
        if self.false_positives:
            self.assertTrue(fp_count > 0)
        jc.close()
        c1.close()
        c0.close()
        self.dropUntilSuccess(self.session, 'table:join09')

if __name__ == '__main__':
    wttest.run()
