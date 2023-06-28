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

# test_sweep05.py
#    Test detection of sessions without recent session sweep.

import time
import wiredtiger, wttest

@wttest.extralongtest('lot of delays')
class test_sweep05(wttest.WiredTigerTestCase):
    '''
    Test detection of sessions without recent session sweep.
    '''
    conn_config = 'statistics=(all)'
    create_params = 'key_format=i,value_format=i'
    table_numkv = 10
    table_uri_format = 'table:test_sweep05_%s'

    def get_stats(self):
        r = dict()
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        r['5min'] = stat_cursor[wiredtiger.stat.conn.no_session_sweep_5min][2]
        r['60min'] = stat_cursor[wiredtiger.stat.conn.no_session_sweep_60min][2]
        stat_cursor.close()
        return r
    
    def assert_stats(self, expected_5min, expected_60min):
        stats = self.get_stats()
        self.assertEquals(stats['5min'], expected_5min)
        self.assertEquals(stats['60min'], expected_60min)

    def create_table(self, name):
        self.session.create(self.table_uri_format % name, self.create_params)
        c = self.session.open_cursor(self.table_uri_format % name, None)
        for k in range(self.table_numkv):
            c[k+1] = 1
        c.close()

    def use_session(self, session, table_name):
        c = session.open_cursor(self.table_uri_format % table_name, None)
        for k in range(self.table_numkv):
            self.assertEquals(c[k+1], 1)
        c.close()

    def test_short(self):
        '''
        The "short" test, focusing on the 5 min violation.
        '''
        self.assert_stats(0, 0)
        self.create_table('table1')
        self.create_table('table2')

        session1 = self.conn.open_session()
        session2 = self.conn.open_session()

        for i in range(0, 4):
            self.use_session(session1, 'table1')
            session1.reset()
            self.assert_stats(0, 0)
            time.sleep(60)
        
        # The default session + session 2 should be marked as rogue.
        time.sleep(120)
        self.ignoreStdoutPatternIfExists('did not run a sweep')
        self.assert_stats(2, 0)

        self.use_session(session2, 'table2')
        session2.reset()
        self.session.reset()

        # At this point, the violation should be cleared, but the counter would not decrease.
        time.sleep(60)
        self.ignoreStdoutPatternIfExists('did not run a sweep')
        self.assert_stats(2, 0)

        # Now let's see if we can log another 5min violation for the two sessions.
        for i in range(0, 4):
            self.use_session(session1, 'table1')
            session1.reset()
            time.sleep(60)
        time.sleep(120)
        self.ignoreStdoutPatternIfExists('did not run a sweep')
        self.assert_stats(4, 0)

    def test_long(self):
        '''
        The "long" test, focusing on the 60 min violation.
        '''
        self.assert_stats(0, 0)
        self.create_table('table1')
        self.create_table('table2')

        session1 = self.conn.open_session()
        session2 = self.conn.open_session()

        for i in range(0, 55):
            self.use_session(session1, 'table1')
            self.use_session(session2, 'table2')
            session1.reset()
            session2.reset()
            time.sleep(60)

        # The default session did not have a sweep for 5 min; others did.
        self.ignoreStdoutPatternIfExists('did not run a sweep')
        self.assert_stats(1, 0)

        for i in range(0, 5):
            self.use_session(session1, 'table1')
            session1.reset()
            time.sleep(60)
        
        # The default session did not have a sweep for a long time; the default session + session 2
        # did not have a sweep for 5 min.
        time.sleep(60)
        self.ignoreStdoutPatternIfExists('did not run a sweep')
        self.assert_stats(2, 1)

if __name__ == '__main__':
    wttest.run()
