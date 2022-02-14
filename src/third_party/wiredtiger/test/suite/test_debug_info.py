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

import wttest

# test_debug_info.py
#    Test WT_CONNECTION::debug_info undocumented feature
class test_debug_info(wttest.WiredTigerTestCase):
    conn_config = 'create,log=(enabled),statistics=(fast)'
    uri = 'file:test_conndump'
    def conn_cursors(self):

        self.session.create(self.uri, 'key_format=i,value_format=i')
        c = self.session.open_cursor(self.uri, None)
        keys = range(1, 101)
        for k in keys:
            c[k] = 1
        c.close()
        c = self.session.open_cursor(self.uri, None)
        c[50]
        self.conn.debug_info('cursors')
        c.close()

    def conn_cursors_special(self, special_uri):
        c = self.session.open_cursor(special_uri, None, None)
        self.conn.debug_info('cursors')
        c.close()

    def test_debug(self):
        with self.expectedStdoutPattern('Data handle dump'):
            self.conn.debug_info('handles')

        with self.expectedStdoutPattern('Active'):
            self.conn.debug_info('sessions')

        with self.expectedStdoutPattern('POSITIONED'):
            self.conn_cursors()

        special = ['backup:', 'log:', 'metadata:', 'statistics:']
        for s in special:
            pat = 'URI: ' + s
            with self.expectedStdoutPattern(pat):
                self.conn_cursors_special(s)

if __name__ == '__main__':
    wttest.run()
