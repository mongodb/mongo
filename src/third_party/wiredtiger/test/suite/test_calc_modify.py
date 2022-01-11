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

import random, string
import wiredtiger, wttest
from wtscenario import make_scenarios

r = random.Random(42) # Make things repeatable

# test_calc_modify.py
#   Test the wiredtiger_calc_modify API
#
# Try many combinations of:
# - data size
# - data randomness ('a' * N, repeated patterns, uniform random)
# - number and type of modifications (add, remove, replace)
# - space between the modifications
#
# Check that wiredtiger_calc_modify finds a set of modifies when the edit
# difference is under the specified limits, and that applying those
# modifications produces the expected result.  If the edit difference is
# larger than the limits, it okay for the call to fail.
class test_calc_modify(wttest.WiredTigerTestCase):
    uri = 'table:test_calc_modify'

    # operation types
    ADD = 1
    REMOVE = 2
    REPLACE = 3

    valuefmt = [
        ('item', dict(valuefmt='u')),
        ('string', dict(valuefmt='S')),
    ]
    scenarios = make_scenarios(valuefmt)

    def mkstring(self, size, repeat_size=1):
        choices = string.ascii_letters + string.digits
        if self.valuefmt == 'S':
            pattern = ''.join(r.choice(choices) for _ in range(repeat_size))
        else:
            pattern = b''.join(bytes([r.choice(choices.encode())]) for _ in range(repeat_size))
        return (pattern * ((size + repeat_size - 1) // repeat_size))[:size]

    def one_test(self, c, k, oldsz, repeatsz, nmod, maxdiff):
        oldv = self.mkstring(oldsz, repeatsz)

        offsets = sorted(r.sample(range(oldsz), nmod))
        modsizes = sorted(r.sample(range(maxdiff), nmod + 1))
        lengths = [modsizes[i+1] - modsizes[i] for i in range(nmod)]
        modtypes = [r.choice((self.ADD, self.REMOVE, self.REPLACE)) for _ in range(nmod)]

        self.pr("offsets: %s" % offsets)
        self.pr("modsizes: %s" % modsizes)
        self.pr("lengths: %s" % lengths)
        self.pr("modtypes: %s" % modtypes)

        orig = oldv
        newv = '' if self.valuefmt == 'S' else b''
        for i in range(1, nmod):
            if offsets[i] - offsets[i - 1] < maxdiff:
                continue
            newv += orig[:(offsets[i]-offsets[i-1])]
            orig = orig[(offsets[i]-offsets[i-1]):]
            if modtypes[i] == self.ADD:
                newv += self.mkstring(lengths[i], r.randint(1, lengths[i]))
            elif modtypes[i] == self.REMOVE:
                orig = orig[lengths[i]:]
            elif modtypes[i] == self.REPLACE:
                newv += self.mkstring(lengths[i], r.randint(1, lengths[i]))
                orig = orig[lengths[i]:]
        newv += orig

        self.pr("oldv: %s" % oldv)
        self.pr("newv: %s" % newv)
        try:
            mods = wiredtiger.wiredtiger_calc_modify(None, oldv, newv, max(maxdiff, nmod * 64), nmod)
            self.pr("calculated mods: %s" % mods)
        except wiredtiger.WiredTigerError:
            # When the data repeats, the algorithm can register the "wrong" repeated sequence.  Retry...
            mods = wiredtiger.wiredtiger_calc_modify(None, oldv, newv, nmod * (64 + repeatsz), nmod)
            self.pr("calculated mods (round 2): %s" % mods)
        self.assertIsNotNone(mods)

        c[k] = oldv
        self.session.begin_transaction()
        c.set_key(k)
        c.modify(mods)
        self.session.commit_transaction()
        self.assertEqual(c[k], newv)

    def test_calc_modify(self):
        self.session.create(self.uri, 'key_format=i,value_format=' + self.valuefmt)
        c = self.session.open_cursor(self.uri)
        for k in range(1000):
            size = r.randint(1000, 10000)
            repeats = r.randint(1, size)
            nmods = r.randint(1, 10)
            maxdiff = r.randint(64, size // 10)
            self.pr("size %s, repeats %s, nmods %s, maxdiff %s" % (size, repeats, nmods, maxdiff))
            self.one_test(c, k, size, repeats, nmods, maxdiff)

if __name__ == '__main__':
    wttest.run()
