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
# test_cursor16.py
#   Cursors: final close of cached cursors
#

import wttest
from wiredtiger import stat
from wtscenario import make_scenarios

class test_cursor16(wttest.WiredTigerTestCase):
    tablename = 'test_cursor16'
    uri_prefix = 'table:' + tablename
    uri_count = 100
    session_count = 100

    conn_config = 'cache_cursors=true,statistics=(fast),in_memory=true'

    scenarios = make_scenarios([
        ('fix', dict(keyfmt='r',valfmt='8t')),
        ('var', dict(keyfmt='r',valfmt='S')),
        ('row', dict(keyfmt='S',valfmt='S')),
    ])

    def getkey(self, n):
        if self.keyfmt == 'r':
            return n + 1
        return str(n)

    def getval(self, n):
        if self.valfmt == '8t':
            return n
        return str(n)

    # Returns the number of cursors cached
    def cached_stats(self):
        stat_cursor = self.session.open_cursor('statistics:', None, None)
        cache = stat_cursor[stat.conn.cursor_cached_count][2]
        stat_cursor.close()
        return cache

    def test_cursor16(self):
        uris = []
        cursors = []
        #self.tty('begin cursors cached=' + str(self.cached_stats()))
        for i in range(0, self.uri_count):
            uri = self.uri_prefix + '-' + str(i)
            uris.append(uri)
            config = 'key_format={},value_format={}'.format(self.keyfmt, self.valfmt)
            self.session.create(uri, config)
            cursor = self.session.open_cursor(uri)
            # We keep the cursors open in the main session, so there
            # will always be a reference to their dhandle, and cached
            # cursors won't get swept.
            cursors.append(cursor)
            for j in range(0, 10):
                cursor[self.getkey(j)] = self.getval(j)

        self.assertEqual(0, self.cached_stats())

        sessions = []
        for i in range(0, self.session_count):
            #if i % 10 == 0:
            #    self.tty('session count=%d cursors cached=%d' %
            #             (i, self.cached_stats()))
            session = self.conn.open_session(self.session_config)
            sessions.append(session)
            for uri in uris:
                cursor = session.open_cursor(uri)
                # spot check, and leaves the cursor positioned
                self.assertEqual(cursor[self.getkey(3)], self.getval(3))
                cursor.close()

        #self.tty('max cursors cached=' + str(self.cached_stats()))
        i = 0
        for session in sessions:
            #if i % 10 == 0:
            #    self.tty('session count=%d cursors cached=%d' %
            #             (self.session_count - i, self.cached_stats()))
            i += 1
            session.close()

        #self.tty('end cursors cached=' + str(self.cached_stats()))
        self.assertEqual(0, self.cached_stats())

if __name__ == '__main__':
    wttest.run()
