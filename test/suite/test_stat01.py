#!/usr/bin/env python
#
# Copyright (c) 2008-2012 WiredTiger, Inc.
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
# test_stat01.py
# 	Statistics operations
#

import wiredtiger, wttest

class test_stat01(wttest.WiredTigerTestCase):
    """
    Test statistics
    """

    tablename = 'test_stat01.wt'
    nentries = 25

    def statstr_to_int(self, str):
        """
        Convert a statistics value string, which may be in either form:
        '12345' or '33M (33604836)'
        """
        parts = str.rpartition('(')
        return int(parts[2].rstrip(')'))

    def check_stats(self, statcursor, mincount, lookfor):
        """
        Do a quick check of the entries in the the stats cursor,
        There should be at least 'mincount' entries,
        and the 'lookfor' string should appear
        """
        stringclass = ''.__class__
        intclass = (0).__class__
        # make sure statistics basically look right
        count = 0
        found = False
        for id, desc, valstr, val in statcursor:
            self.assertEqual(type(desc), stringclass)
            self.assertEqual(type(valstr), stringclass)
            self.assertEqual(type(val), intclass)
            self.assertEqual(val, self.statstr_to_int(valstr))
            self.printVerbose(2, '  stat: \'' + desc + '\', \'' +
                              valstr + '\', ' + str(val))
            count += 1
            if desc == lookfor:
                found = True
        self.assertTrue(count > mincount)
        self.assertTrue(found, 'in stats, did not see: ' + lookfor)

    def test_statistics(self):
        extra_params = ',allocation_size=512,internal_page_max=16384,leaf_page_max=131072'
        self.session.create('table:' + self.tablename, 'key_format=S,value_format=S' + extra_params)
        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        value = ""
        for i in range(0, self.nentries):
            key = str(i)
            value = value + key + value # size grows exponentially
            cursor.set_key(key)
            cursor.set_value(value)
            cursor.insert()
        cursor.close()

        self.printVerbose(2, 'overall database stats:')
        allstat_cursor = self.session.open_cursor('statistics:', None, None)
        self.check_stats(allstat_cursor, 10, 'blocks written to a file')

        # See that we can get a specific stat value by its key,
        # and verify that its entry is self-consistent
        allstat_cursor.set_key(wiredtiger.stat.block_write)
        self.assertEqual(allstat_cursor.search(), 0)
        values = allstat_cursor.get_values()
        self.assertEqual(values[0], 'blocks written to a file')
        val = self.statstr_to_int(values[1])
        self.assertEqual(val, values[2])
        allstat_cursor.close()

        self.printVerbose(2, 'file specific stats:')
        filestat_cursor = self.session.open_cursor('statistics:file:' + self.tablename + ".wt", None, None)
        self.check_stats(filestat_cursor, 10, 'overflow pages')

        # See that we can get a specific stat value by its key,
        # and verify that its entry is self-consistent
        filestat_cursor.set_key(wiredtiger.filestat.overflow)
        self.assertEqual(filestat_cursor.search(), 0)
        values = filestat_cursor.get_values()
        self.assertEqual(values[0], 'overflow pages')
        val = self.statstr_to_int(values[1])
        self.assertEqual(val, values[2])
        filestat_cursor.close()

        self.assertRaises(wiredtiger.WiredTigerError,
	    lambda: self.session.open_cursor(
	    'statistics:file:DoesNotExist', None, None))

if __name__ == '__main__':
    wttest.run()
