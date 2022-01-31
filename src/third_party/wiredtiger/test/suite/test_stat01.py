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

import helper, wiredtiger, wttest
from wiredtiger import stat
from wtdataset import SimpleDataSet, simple_key
from wtscenario import make_scenarios

# test_stat01.py
#    Statistics operations
class test_stat01(wttest.WiredTigerTestCase):
    """
    Test statistics
    """

    config = 'internal_page_max=4K,leaf_page_max=8K'
    nentries = 25

    types = [
        ('file', dict(uri='file:test_stat01.wt')),
        ('table', dict(uri='table:test_stat01.wt'))
    ]
    keyfmt = [
        ('column', dict(keyfmt='r', valfmt='S')),
        ('column-fix', dict(keyfmt='r', valfmt='8t')),
        ('string-row', dict(keyfmt='S', valfmt='S')),
    ]
    scenarios = make_scenarios(types, keyfmt)

    conn_config = 'statistics=(all)'

    def statstr_to_int(self, str):
        """
        Convert a statistics value string, which may be in either form:
        '12345' or '33M (33604836)'
        """
        parts = str.rpartition('(')
        return int(parts[2].rstrip(')'))

    # Do a quick check of the entries in the stats cursor, the "lookfor"
    # string should appear with a minimum value of least "min".
    def check_stats(self, statcursor, min, lookfor):
        stringclass = ''.__class__
        intclass = (0).__class__

        # Reset the cursor, we're called multiple times.
        statcursor.reset()

        found = False
        foundval = 0
        for id, desc, valstr, val in statcursor:
            self.assertEqual(type(desc), stringclass)
            self.assertEqual(type(valstr), stringclass)
            self.assertEqual(type(val), intclass)
            self.assertEqual(val, self.statstr_to_int(valstr))
            self.printVerbose(2, '  stat: \'' + desc + '\', \'' +
                              valstr + '\', ' + str(val))
            if desc == lookfor:
                found = True
                foundval = val

        self.assertTrue(found, 'in stats, did not see: ' + lookfor)
        self.assertTrue(foundval >= min)

    # Test simple connection statistics.
    def test_basic_conn_stats(self):
        # Build an object and force some writes.
        ds = SimpleDataSet(self, self.uri, 1000,
                      config=self.config, key_format=self.keyfmt, value_format = self.valfmt)
        ds.populate()
        self.session.checkpoint(None)

        # See that we can get a specific stat value by its key and verify its
        # entry is self-consistent.
        allstat_cursor = self.session.open_cursor('statistics:', None, None)
        self.check_stats(allstat_cursor, 10, 'block-manager: blocks written')

        values = allstat_cursor[stat.conn.block_write]
        self.assertEqual(values[0], 'block-manager: blocks written')
        val = self.statstr_to_int(values[1])
        self.assertEqual(val, values[2])
        allstat_cursor.close()

    # Test simple object statistics.
    def test_basic_data_source_stats(self):
        # Build an object.
        config = self.config + ',key_format=' + self.keyfmt + \
            ',value_format=S'
        self.session.create(self.uri, config)
        cursor = self.session.open_cursor(self.uri, None, None)
        value = ""
        for i in range(1, self.nentries):
            value = value + 1000 * "a"
            cursor[simple_key(cursor, i)] = value
        cursor.close()

        # Force the object to disk, otherwise we can't check the overflow count.
        self.reopen_conn()

        # See that we can get a specific stat value by its key and verify its
        # entry is self-consistent.
        cursor = self.session.open_cursor('statistics:' + self.uri, None, None)
        self.check_stats(cursor, 8192, 'btree: maximum leaf page size')
        self.check_stats(cursor, 4096, 'btree: maximum internal page size')
        self.check_stats(cursor, 10, 'btree: overflow pages')

        values = cursor[stat.dsrc.btree_overflow]
        self.assertEqual(values[0], 'btree: overflow pages')
        val = self.statstr_to_int(values[1])
        self.assertEqual(val, values[2])
        cursor.close()

        cursor = self.session.open_cursor(
            'statistics:' + self.uri, None, "statistics=(size)")
        values = cursor[stat.dsrc.block_size]
        self.assertNotEqual(values[2], 0)
        cursor.close()

    # Test simple per-checkpoint statistics.
    def test_checkpoint_stats(self):
        ds = SimpleDataSet(self, self.uri, self.nentries,
            config=self.config, key_format=self.keyfmt)
        for name in ('first', 'second', 'third'):
            ds.populate()
            self.session.checkpoint('name=' + name)
            cursor = self.session.open_cursor(
                'statistics:' + self.uri, None, 'checkpoint=' + name)
            self.assertEqual(
                cursor[stat.dsrc.btree_entries][2], self.nentries)
            cursor.close()

    def test_missing_file_stats(self):
        self.assertRaises(wiredtiger.WiredTigerError, lambda:
            self.session.open_cursor('statistics:file:DoesNotExist'))

if __name__ == '__main__':
    wttest.run()
