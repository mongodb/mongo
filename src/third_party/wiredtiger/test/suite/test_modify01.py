#!/usr/bin/env python3
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

from modify_utils import create_mods
from wtscenario import make_scenarios

# Test the wiredtiger_calc_modify API.
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
class test_modify01(wttest.WiredTigerTestCase):
    uri = 'table:test_modify01'

    valuefmt = [
        ('item', dict(valuefmt='u')),
        ('string', dict(valuefmt='S')),
    ]

    scenarios = make_scenarios(valuefmt)

    def test_modify01(self):
        r = random.Random(42) # Make things repeatable

        self.session.create(self.uri, 'key_format=i,value_format=' + self.valuefmt)

        c = self.session.open_cursor(self.uri)
        for k in range(1000):
            size = r.randint(1000, 10000)
            repeats = r.randint(1, size)
            nmods = r.randint(1, 10)
            maxdiff = r.randint(64, size // 10)

            self.pr("size %s, repeats %s, nmods %s, maxdiff %s" % (size, repeats, nmods, maxdiff))
            (oldv, mods, newv) = create_mods(r, size, repeats, nmods, maxdiff, self.valuefmt)

            self.assertIsNotNone(mods)

            c[k] = oldv
            self.session.begin_transaction()
            c.set_key(k)
            c.modify(mods)

            # Use a timestamp for compatibility with the disagg hook.
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(k+1))
            self.assertEqual(c[k], newv)
