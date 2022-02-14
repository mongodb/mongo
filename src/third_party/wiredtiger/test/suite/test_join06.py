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
from wtscenario import make_scenarios

# test_join06.py
#    Join operations
# Joins with a read-uncommitted
class test_join06(wttest.WiredTigerTestCase):
    nentries = 1000

    isoscen = [
        ('isolation_read_uncommitted', dict(isolation='read-uncommitted')),
        ('isolation_read_committed', dict(isolation='read-committed')),
        ('isolation_default', dict(isolation='')),
        ('isolation_snapshot', dict(isolation='snapshot'))
    ]

    bloomscen = [
        ('bloom', dict(bloom=True)),
        ('nobloom', dict(bloom=False))
    ]

    scenarios = make_scenarios(isoscen, bloomscen)

    def gen_values(self, i):
        s = str(i)                    # 345 => "345"
        f = s[0:1] + s[0:1] + s[0:1]  # 345 => "333"
        return [s, f]

    def gen_values2(self, i):
        s = str(i)                    # 345 => "345"
        l = s[-1:] + s[-1:] + s[-1:]  # 345 => "555"
        return [s, l]

    def populate(self, s, gen_values):
        c = s.open_cursor('table:join06', None, None)
        for i in range(0, self.nentries):
            c.set_key(i)
            c.set_value(*gen_values(i))
            c.insert()
        c.close()

    # Common function for testing the most basic functionality
    # of joins
    def test_join(self):
        self.session.create('table:join06',
                            'columns=(k,v0,v1),key_format=i,value_format=SS')
        self.session.create('index:join06:index0','columns=(v0)')
        self.session.create('index:join06:index1','columns=(v1)')

        self.populate(self.session, self.gen_values)

        # TODO: needed?
        #self.reopen_conn()

        if self.isolation != '':
            self.session.begin_transaction('isolation=' + self.isolation)

        jc = self.session.open_cursor('join:table:join06', None, None)
        c0 = self.session.open_cursor('index:join06:index0', None, None)
        c0.set_key('520')
        self.assertEquals(0, c0.search())
        self.session.join(jc, c0, 'compare=ge')

        joinconfig = 'compare=eq'
        if self.bloom:
            joinconfig += ',strategy=bloom,count=1000'
        c1 = self.session.open_cursor('index:join06:index1', None, None)
        c1.set_key('555')
        self.assertEquals(0, c1.search())
        self.session.join(jc, c1, joinconfig)

        if self.isolation == 'read-uncommitted' and self.bloom:
            # Make sure that read-uncommitted with Bloom is not allowed.
            # This is detected on the first next() operation.
            msg = '/cannot be used with read-uncommitted/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: jc.next(), msg)
            return

        # Changes made in another session may or may not be visible to us,
        # depending on the isolation level.
        if self.isolation == 'read-uncommitted':
            # isolation level is read-uncommitted, so we will see
            # additions deletions made in our other session.
            mbr = set(range(525,1000,10)) | set(range(55,100,10)) | set([520])
        else:
            # default isolation level, so we should see a consistent
            # set at the time we begin iteration.
            mbr = set(range(520,600)) | set(range(53,60))

        altered = False
        while jc.next() == 0:
            [k] = jc.get_keys()
            [v0,v1] = jc.get_values()
            #self.tty('GOT: ' + str(k) + ': ' + str(jc.get_values()))
            if altered and self.isolation == 'read-uncommitted':
                self.assertEquals(self.gen_values2(k), [v0, v1])
            else:
                self.assertEquals(self.gen_values(k), [v0, v1])
            if not k in mbr:
                self.tty('**** ERROR: result ' + str(k) + ' is not in: ' +
                         str(mbr))
            self.assertTrue(k in mbr)
            mbr.remove(k)

            # In another session, we remove entries for keys ending in 6,
            # and add entries for keys ending in 5.  Depending on the
            # isolation level for the transaction, these changes may or
            # may not be visible for the original session.
            if not altered:
                s = self.conn.open_session(None)
                s.begin_transaction(None)
                self.populate(s, self.gen_values2)
                s.commit_transaction()
                s.close()
                altered = True

        if len(mbr) != 0:
            self.tty('**** ERROR: did not see these: ' + str(mbr))
        self.assertEquals(0, len(mbr))

        jc.close()
        c1.close()
        c0.close()
        if self.isolation != '':
            self.session.commit_transaction()
        self.session.drop('table:join06')

if __name__ == '__main__':
    wttest.run()
