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
# test_hs04.py
#   Test file_max configuration and reconfiguration for the history store table.
#

import wiredtiger, wttest
from wtscenario import make_scenarios

# Taken from src/include/misc.h.
WT_MB = 1048576

class test_hs04(wttest.WiredTigerTestCase):
    uri = 'table:hs_04'
    in_memory_values = [
        ('false', dict(in_memory=False)),
        ('none', dict(in_memory=None)),
        ('true', dict(in_memory=True))
    ]
    init_file_max_values = [
        ('default', dict(init_file_max=None, init_stat_val=0)),
        ('non-zero', dict(init_file_max='100MB', init_stat_val=(WT_MB * 100))),
        ('zero', dict(init_file_max='0', init_stat_val=0))
    ]
    reconfig_file_max_values = [
        ('non-zero', dict(reconfig_file_max='100MB',
                          reconfig_stat_val=(WT_MB * 100))),
        ('too-low', dict(reconfig_file_max='99MB', reconfig_stat_val=None)),
        ('zero', dict(reconfig_file_max='0', reconfig_stat_val=0))
    ]
    scenarios = make_scenarios(init_file_max_values, reconfig_file_max_values,
                               in_memory_values)

    def conn_config(self):
        config = 'statistics=(fast)'
        if self.init_file_max is not None:
            config += ',history_store=(file_max={})'.format(self.init_file_max)
        if self.in_memory is not None:
            config += ',in_memory=' + ('true' if self.in_memory else 'false')
        return config

    def get_stat(self, stat):
        stat_cursor = self.session.open_cursor('statistics:')
        val = stat_cursor[stat][2]
        stat_cursor.close()
        return val

    def test_hs(self):
        self.session.create(self.uri, 'key_format=S,value_format=S')

        if self.in_memory:
            # For in-memory configurations, we simply ignore any history store
            # related configuration.
            self.assertEqual(
                self.get_stat(wiredtiger.stat.conn.cache_hs_ondisk_max),
                0)
        else:
            self.assertEqual(
                self.get_stat(wiredtiger.stat.conn.cache_hs_ondisk_max),
                self.init_stat_val)

        reconfigure = lambda: self.conn.reconfigure(
            'history_store=(file_max={})'.format(self.reconfig_file_max))

        # We expect an error when the statistic value is None because the value
        # is out of range.
        if self.reconfig_stat_val is None:
            self.assertRaisesWithMessage(
                wiredtiger.WiredTigerError, reconfigure, '/below minimum/')
            return

        reconfigure()

        if self.in_memory:
            self.assertEqual(
                self.get_stat(wiredtiger.stat.conn.cache_hs_ondisk_max),
                0)
        else:
            self.assertEqual(
                self.get_stat(wiredtiger.stat.conn.cache_hs_ondisk_max),
                self.reconfig_stat_val)

if __name__ == '__main__':
    wttest.run()
