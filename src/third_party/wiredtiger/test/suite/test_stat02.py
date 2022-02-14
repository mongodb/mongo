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

import itertools, wiredtiger, wttest
from wtdataset import SimpleDataSet, SimpleLSMDataSet, ComplexDataSet, ComplexLSMDataSet
from wtscenario import make_scenarios
from wiredtiger import stat

# test_stat02.py
#    Statistics cursor configurations.
class test_stat_cursor_config(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_config'
    uri = [
        ('file',  dict(uri='file:' + pfx, dataset=SimpleDataSet)),
        ('table', dict(uri='table:' + pfx, dataset=SimpleDataSet)),
        ('table-lsm', dict(uri='table:' + pfx, dataset=SimpleLSMDataSet)),
        ('complex', dict(uri='table:' + pfx, dataset=ComplexDataSet)),
        ('complex-lsm', dict(uri='table:' + pfx, dataset=ComplexLSMDataSet))
    ]
    data_config = [
        ('none', dict(data_config='none', ok=[])),
        ( 'all',  dict(data_config='all', ok=['empty', 'fast', 'all', 'size'])),
        ('fast', dict(data_config='fast', ok=['empty', 'fast', 'size']))
    ]
    cursor_config = [
        ('empty', dict(cursor_config='empty')),
        ( 'all', dict(cursor_config='all')),
        ('fast', dict(cursor_config='fast')),
        ('size', dict(cursor_config='size'))
    ]

    scenarios = make_scenarios(uri, data_config, cursor_config)

    # Turn on statistics for this test.
    def conn_config(self):
        return 'statistics=(%s)' % self.data_config

    # For each database/cursor configuration, confirm the right combinations
    # succeed or fail.
    def test_stat_cursor_config(self):
        self.dataset(self, self.uri, 100).populate()
        config = 'statistics=('
        if self.cursor_config != 'empty':
            config = config + self.cursor_config
        config = config + ')'
        if self.ok and self.cursor_config in self.ok:
            self.session.open_cursor('statistics:', None, config)
        else:
            msg = '/database statistics configuration/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError, lambda:
                self.session.open_cursor('statistics:', None, config), msg)

# Test the connection "clear" configuration.
class test_stat_cursor_conn_clear(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_conn_clear'
    conn_config = 'statistics=(all)'

    def test_stat_cursor_conn_clear(self):
        uri = 'table:' + self.pfx
        ComplexDataSet(self, uri, 100).populate()

        # cursor_insert should clear
        # cache_bytes_dirty should not clear
        cursor = self.session.open_cursor(
            'statistics:', None, 'statistics=(all,clear)')
        self.assertGreater(cursor[stat.conn.cache_bytes_dirty][2], 0)
        self.assertGreater(cursor[stat.conn.cursor_insert][2], 0)
        cursor = self.session.open_cursor(
            'statistics:', None, 'statistics=(all,clear)')
        self.assertGreater(cursor[stat.conn.cache_bytes_dirty][2], 0)
        self.assertEqual(cursor[stat.conn.cursor_insert][2], 0)

# Test the data-source "clear" configuration.
class test_stat_cursor_dsrc_clear(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_dsrc_clear'

    uri = [
        ('dsrc_clear_1',  dict(uri='file:' + pfx, dataset=SimpleDataSet)),
        ('dsrc_clear_2', dict(uri='table:' + pfx, dataset=SimpleDataSet)),
        ('dsrc_clear_3', dict(uri='table:' + pfx, dataset=ComplexDataSet)),
        ('dsrc_clear_4', dict(uri='table:' + pfx, dataset=ComplexLSMDataSet))
    ]

    scenarios = make_scenarios(uri)
    conn_config = 'statistics=(all)'

    def test_stat_cursor_dsrc_clear(self):
        self.dataset(self, self.uri, 100).populate()

        # cursor_insert should clear
        #
        # We can't easily test data-source items that shouldn't clear: as I
        # write this, session_cursor_open is the only such item, and it will
        # change to account for the statistics cursors we open here.
        cursor = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(all,clear)')
        self.assertGreater(cursor[stat.dsrc.cursor_insert][2], 0)
        cursor = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(all,clear)')
        self.assertEqual(cursor[stat.dsrc.cursor_insert][2], 0)

# Test the "fast" configuration.
class test_stat_cursor_fast(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_fast'

    uri = [
        ('fast_1',  dict(uri='file:' + pfx, dataset=SimpleDataSet)),
        ('fast_2', dict(uri='table:' + pfx, dataset=SimpleDataSet)),
        ('fast_3', dict(uri='table:' + pfx, dataset=ComplexDataSet)),
        ('fast_4', dict(uri='table:' + pfx, dataset=ComplexLSMDataSet))
    ]

    scenarios = make_scenarios(uri)
    conn_config = 'statistics=(all)'

    def test_stat_cursor_fast(self):
        self.dataset(self, self.uri, 100).populate()

        # A "fast" cursor shouldn't see the underlying btree statistics.
        # Check "fast" first, otherwise we get a copy of the statistics
        # we generated in the "all" call, they just aren't updated.
        cursor = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(fast)')
        self.assertEqual(cursor[stat.dsrc.btree_entries][2], 0)
        cursor = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(all)')
        self.assertGreater(cursor[stat.dsrc.btree_entries][2], 0)

# Test connection error combinations.
class test_stat_cursor_conn_error(wttest.WiredTigerTestCase):
    def setUpConnectionOpen(self, dir):
        return None
    def setUpSessionOpen(self, conn):
        return None

    def test_stat_cursor_conn_error(self):
        args = ['none', 'all', 'fast']
        for i in list(itertools.permutations(args, 2)):
            config = 'create,statistics=(' + i[0] + ',' + i[1] + ')'
            msg = '/Only one of/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.wiredtiger_open('.', config), msg)

# Test data-source error combinations.
class test_stat_cursor_dsrc_error(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_dsrc_error'

    uri = [
        ('dsrc_error_1',  dict(uri='file:' + pfx, dataset=SimpleDataSet)),
        ('dsrc_error_2', dict(uri='table:' + pfx, dataset=SimpleDataSet)),
        ('dsrc_error_3', dict(uri='table:' + pfx, dataset=ComplexDataSet)),
        ('dsrc_error_4', dict(uri='table:' + pfx, dataset=ComplexLSMDataSet))
    ]

    scenarios = make_scenarios(uri)
    conn_config = 'statistics=(all)'

    def test_stat_cursor_dsrc_error(self):
        self.dataset(self, self.uri, 100).populate()
        args = ['all', 'fast']
        for i in list(itertools.permutations(args, 2)):
            config = 'statistics=(' + i[0] + ',' + i[1] + ')'
            msg = '/Only one of/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.open_cursor(
                'statistics:' + self.uri, None, config), msg)

# Test data-source cache walk statistics
class test_stat_cursor_dsrc_cache_walk(wttest.WiredTigerTestCase):
    uri = 'file:test_stat_cursor_dsrc_cache_walk'

    conn_config = 'statistics=(none)'

    def test_stat_cursor_dsrc_cache_walk(self):
        SimpleDataSet(self, self.uri, 100).populate()
        # Ensure that it's an error to get cache_walk stats if none is set
        msg = '/doesn\'t match the database statistics/'
        self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
            lambda: self.session.open_cursor(
            'statistics:' + self.uri, None, None), msg)

        # Test configurations that are valid but should not collect
        # cache walk information. Do these first since the cache walk
        # statistics are mostly marked as not cleared - so once they are
        # populated the values will always be returned
        self.conn.reconfigure('statistics=(cache_walk,fast,clear)')
        c = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(fast)')
        self.assertEqual(c[stat.dsrc.cache_state_root_size][2], 0)
        c.close()

        self.conn.reconfigure('statistics=(all,clear)')
        c = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(fast)')
        self.assertEqual(c[stat.dsrc.cache_state_root_size][2], 0)
        c.close()

        self.conn.reconfigure('statistics=(cache_walk,fast,clear)')
        c = self.session.open_cursor('statistics:' + self.uri, None, None)
        self.assertGreater(c[stat.dsrc.cache_state_root_size][2], 0)
        # Verify that cache_walk didn't imply tree_walk
        self.assertEqual(c[stat.dsrc.btree_entries][2], 0)
        c.close()

        self.conn.reconfigure('statistics=(cache_walk,tree_walk,fast,clear)')
        c = self.session.open_cursor('statistics:' + self.uri, None, None)
        self.assertGreater(c[stat.dsrc.cache_state_root_size][2], 0)
        # Verify that cache_walk didn't exclude tree_walk
        self.assertGreater(c[stat.dsrc.btree_entries][2], 0)
        c.close()

        self.conn.reconfigure('statistics=(all,clear)')
        c = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(all)')
        self.assertGreater(c[stat.dsrc.cache_state_root_size][2], 0)
        self.assertGreater(c[stat.dsrc.btree_entries][2], 0)
        c.close()

        # Verify that cache and tree walk can operate independently
        self.conn.reconfigure('statistics=(all,clear)')
        c = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(cache_walk,fast)')
        self.assertGreater(c[stat.dsrc.cache_state_root_size][2], 0)
        self.assertEqual(c[stat.dsrc.btree_entries][2], 0)
        c.close()

        self.conn.reconfigure('statistics=(all,clear)')
        c = self.session.open_cursor(
            'statistics:' + self.uri, None, 'statistics=(tree_walk,fast)')
        # Don't check the cache walk stats for empty - they won't be cleared
        self.assertGreater(c[stat.dsrc.btree_entries][2], 0)
        c.close()

if __name__ == '__main__':
    wttest.run()
