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

import itertools, wiredtiger, wttest
from suite_subprocess import suite_subprocess
from wiredtiger import stat

from helper import complex_populate, complex_populate_lsm, simple_populate
from helper import key_populate, complex_value_populate, value_populate
from helper import complex_populate_colgroup_count, complex_populate_index_count
from helper import complex_populate_colgroup_name, complex_populate_index_name
from wtscenario import multiply_scenarios, number_scenarios

# test_stat03.py
#    Statistics reset test.
class test_stat_cursor_reset(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_reset'
    uri = [
        ('file-simple',
            dict(uri='file:' + pfx, pop=simple_populate)),
        ('table-simple',
            dict(uri='table:' + pfx, pop=simple_populate)),
        ('table-complex',
            dict(uri='table:' + pfx, pop=complex_populate)),
        ('table-complex-lsm',
            dict(uri='table:' + pfx, pop=complex_populate_lsm)),
    ]

    scenarios = number_scenarios(multiply_scenarios('.', uri))
    conn_config = 'statistics=(all)'

    def stat_cursor(self, uri):
        return self.session.open_cursor(
            'statistics:' + uri, None, 'statistics=(all)')

    def test_stat_cursor_reset(self):
        # The number of btree_entries reported is influenced by the
        # number of column groups and indices.  Each insert will have
        # a multiplied effect.
        if self.pop == simple_populate:
            multiplier = 1   # no declared colgroup is like one big colgroup
        else:
            multiplier = complex_populate_colgroup_count() + \
                         complex_populate_index_count()
        n = 100
        self.pop(self, self.uri, 'key_format=S', n)
        statc = self.stat_cursor(self.uri)
        self.assertEqual(statc[stat.dsrc.btree_entries][2], n * multiplier)

        c = self.session.open_cursor(self.uri)
        c.set_key(key_populate(c, 200))
        if self.pop == simple_populate:
            c.set_value(value_populate(c, 200))
        else:
            c.set_value(tuple(complex_value_populate(c, 200)))
        c.insert()

        # Test that cursor reset re-loads the values.
        self.assertEqual(statc[stat.dsrc.btree_entries][2], n * multiplier)
        statc.reset()
        n += 1
        self.assertEqual(statc[stat.dsrc.btree_entries][2], n * multiplier)

        # For applications with indices and/or column groups, verify
        # that there is a way to count the base number of entries.
        if self.pop != simple_populate:
            statc.close()
            statc = self.stat_cursor(
                complex_populate_index_name(self, self.uri, 0))
            self.assertEqual(statc[stat.dsrc.btree_entries][2], n)
            statc.close()
            statc = self.stat_cursor(
                complex_populate_colgroup_name(self, self.uri, 0))
            self.assertEqual(statc[stat.dsrc.btree_entries][2], n)
        statc.close()


if __name__ == '__main__':
    wttest.run()
