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

import random, string, sys
import wiredtiger, wttest

from helper_disagg import DisaggConfigMixin, disagg_test_class, gen_disagg_storages
from modify_utils import create_mods, create_value
from wtscenario import make_scenarios

# Test that a modify remains valid across a checkpoint update.
@disagg_test_class
class test_layered_modify01(wttest.WiredTigerTestCase, DisaggConfigMixin):
    conn_base_config = 'disaggregated=(page_log=palm),'
    disagg_storages = gen_disagg_storages('test_layered_modify01', disagg_only=True)
    uri = 'layered:test_layered_modify01'

    valuefmt = [
        ('item', dict(valuefmt='u')),
        ('string', dict(valuefmt='S')),
    ]
    scenarios = make_scenarios(disagg_storages, valuefmt)

    def conn_config(self):
        return self.conn_base_config + f'disaggregated=(role="leader"),'

    def test_layered_modify01(self):
        r = random.Random(42) # Make things repeatable

        self.session.create(self.uri, 'key_format=i,value_format=' + self.valuefmt)
        c = self.session.open_cursor(self.uri)

        old_vals = []
        for k in range(1000):
            size = r.randint(1000, 10000)
            repeats = r.randint(1, size)
            nmods = r.randint(1, 10)
            maxdiff = r.randint(64, size // 10)
            oldv = create_value(r, size, repeats, self.valuefmt)

            c[k] = oldv
            old_vals.append(oldv)

        self.session.checkpoint()

        # We don't need a second WT to test what we want -- reopen the existing
        # one and tell it to grab the latest checkpoint.
        self.reopen_conn(config=self.conn_base_config + f'disaggregated=(role="follower",checkpoint_meta="{self.disagg_get_complete_checkpoint_meta()}")')
        c = self.session.open_cursor(self.uri)

        for k in range(1000):
            (oldv, mods, newv) = create_mods(r, size, repeats, nmods, maxdiff, self.valuefmt, old_vals[k])
            self.assertIsNotNone(mods)

            self.session.begin_transaction()
            c.set_key(k)
            ret = c.modify(mods)
            self.assertEqual(ret, 0)
            self.session.commit_transaction("commit_timestamp=" + self.timestamp_str(k+1))
            self.assertEqual(c[k], newv)
