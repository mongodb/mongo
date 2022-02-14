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
#
# [TEST_TAGS]
# cursors:statistics
# [END_TAGS]
import wttest
from wiredtiger import stat

from wtdataset import SimpleDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios

# test_stat03.py
#    Statistics reset test.
class test_stat_cursor_reset(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_reset'
    uri = [
        ('file-simple-row', dict(uri='file:' + pfx, dataset=SimpleDataSet, kf='S', vf='S')),
        ('file-simple-var', dict(uri='file:' + pfx, dataset=SimpleDataSet, kf='r', vf='S')),
        ('file-simple-fix', dict(uri='file:' + pfx, dataset=SimpleDataSet, kf='r', vf='8t')),
        ('table-simple-row', dict(uri='table:' + pfx, dataset=SimpleDataSet, kf='S', vf='S')),
        ('table-simple-var', dict(uri='table:' + pfx, dataset=SimpleDataSet, kf='r', vf='S')),
        ('table-simple-fix', dict(uri='table:' + pfx, dataset=SimpleDataSet, kf='r', vf='8t')),
        # The complex data sets ignore any passed-in value format.
        ('table-complex-row', dict(uri='table:' + pfx, dataset=ComplexDataSet, kf='S', vf=None)),
        ('table-complex-var', dict(uri='table:' + pfx, dataset=ComplexDataSet, kf='r', vf=None)),
        ('table-complex-lsm', dict(uri='table:' + pfx,
            dataset=ComplexLSMDataSet, kf='S', vf=None))
    ]

    scenarios = make_scenarios(uri)
    conn_config = 'statistics=(all)'

    def stat_cursor(self, uri):
        return self.session.open_cursor(
            'statistics:' + uri, None, 'statistics=(all)')

    def test_stat_cursor_reset(self):
        n = 100
        ds = self.dataset(self, self.uri, n, key_format=self.kf, value_format=self.vf)
        ds.populate()

        # The number of btree_entries reported is influenced by the
        # number of column groups and indices.  Each insert will have
        # a multiplied effect.
        if self.dataset == SimpleDataSet:
            multiplier = 1   # no declared colgroup is like one big colgroup
        else:
            multiplier = ds.colgroup_count() +  ds.index_count()
        statc = self.stat_cursor(self.uri)
        self.assertEqual(statc[stat.dsrc.btree_entries][2], n * multiplier)

        c = self.session.open_cursor(self.uri)
        c.set_key(ds.key(200))
        c.set_value(ds.value(200))
        c.insert()

        # Test that cursor reset re-loads the values.
        self.assertEqual(statc[stat.dsrc.btree_entries][2], n * multiplier)
        statc.reset()
        n += 1
        self.assertEqual(statc[stat.dsrc.btree_entries][2], n * multiplier)

        # For applications with indices and/or column groups, verify
        # that there is a way to count the base number of entries.
        if self.dataset != SimpleDataSet:
            statc.close()
            statc = self.stat_cursor(ds.index_name(0))
            self.assertEqual(statc[stat.dsrc.btree_entries][2], n)
            statc.close()
            statc = self.stat_cursor(ds.colgroup_name(0))
            self.assertEqual(statc[stat.dsrc.btree_entries][2], n)
        statc.close()

if __name__ == '__main__':
    wttest.run()
