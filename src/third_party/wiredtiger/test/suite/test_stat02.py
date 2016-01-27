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
from wtscenario import multiply_scenarios, number_scenarios
from wiredtiger import stat
from helper import complex_populate, complex_populate_lsm, simple_populate

# test_stat02.py
#    Statistics cursor configurations.
class test_stat_cursor_config(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_config'
    uri = [
        ('file',  dict(uri='file:' + pfx, pop=simple_populate, cfg='')),
        ('table', dict(uri='table:' + pfx, pop=simple_populate, cfg='')),
        ('table-lsm',
            dict(uri='table:' + pfx, pop=simple_populate, cfg=',type=lsm')),
        ('complex', dict(uri='table:' + pfx, pop=complex_populate, cfg='')),
        ('complex-lsm',
            dict(uri='table:' + pfx, pop=complex_populate_lsm, cfg=''))
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

    scenarios = number_scenarios(
        multiply_scenarios('.', uri, data_config, cursor_config))

    # Turn on statistics for this test.
    def conn_config(self, dir):
        return 'statistics=(%s)' % self.data_config

    # For each database/cursor configuration, confirm the right combinations
    # succeed or fail.
    def test_stat_cursor_config(self):
        self.pop(self, self.uri, 'key_format=S' + self.cfg, 100)
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
        complex_populate(self, uri, 'key_format=S', 100)

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
        ('1',  dict(uri='file:' + pfx, pop=simple_populate)),
        ('2', dict(uri='table:' + pfx, pop=simple_populate)),
        ('3', dict(uri='table:' + pfx, pop=complex_populate)),
        ('4', dict(uri='table:' + pfx, pop=complex_populate_lsm))
    ]

    scenarios = number_scenarios(multiply_scenarios('.', uri))
    conn_config = 'statistics=(all)'

    def test_stat_cursor_dsrc_clear(self):
        self.pop(self, self.uri, 'key_format=S', 100)

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
        ('1',  dict(uri='file:' + pfx, pop=simple_populate)),
        ('2', dict(uri='table:' + pfx, pop=simple_populate)),
        ('3', dict(uri='table:' + pfx, pop=complex_populate)),
        ('4', dict(uri='table:' + pfx, pop=complex_populate_lsm))
    ]

    scenarios = number_scenarios(multiply_scenarios('.', uri))
    conn_config = 'statistics=(all)'

    def test_stat_cursor_fast(self):
        self.pop(self, self.uri, 'key_format=S', 100)

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
            msg = '/only one statistics configuration value/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.wiredtiger_open('.', config), msg)


# Test data-source error combinations.
class test_stat_cursor_dsrc_error(wttest.WiredTigerTestCase):
    pfx = 'test_stat_cursor_dsrc_error'

    uri = [
        ('1',  dict(uri='file:' + pfx, pop=simple_populate)),
        ('2', dict(uri='table:' + pfx, pop=simple_populate)),
        ('3', dict(uri='table:' + pfx, pop=complex_populate)),
        ('4', dict(uri='table:' + pfx, pop=complex_populate_lsm))
    ]

    scenarios = number_scenarios(multiply_scenarios('.', uri))
    conn_config = 'statistics=(all)'

    def test_stat_cursor_dsrc_error(self):
        self.pop(self, self.uri, 'key_format=S', 100)
        args = ['all', 'fast']
        for i in list(itertools.permutations(args, 2)):
            config = 'statistics=(' + i[0] + ',' + i[1] + ')'
            msg = '/only one statistics configuration value/'
            self.assertRaisesWithMessage(wiredtiger.WiredTigerError,
                lambda: self.session.open_cursor(
                'statistics:' + self.uri, None, config), msg)


if __name__ == '__main__':
    wttest.run()
