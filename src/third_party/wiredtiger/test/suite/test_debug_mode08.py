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

import wttest, test_base03

# test_debug_mode08.py
#   Test the debug mode setting for cursor_copy.
# There's no great way to detect that cursor_copy is really working, as
# the extra malloc/free that WT does cannot easily be detected.  However,
# it's useful to run some tests with the mode enabled. We make a subclass
# of test_base03, so we'll inherit those tests to be run with the debug mode
# configuration enabled.
class test_debug_mode08(test_base03.test_base03):
    conn_config = 'debug_mode=(cursor_copy=true)'
    uri = 'file:test_debug_mode08'

    def test_reconfig(self):
        ''' Test reconfigure with some minimal cursor activity. '''
        self.session.create(self.uri, 'key_format=s,value_format=s')
        cursor = self.session.open_cursor(self.uri, None)
        cursor['key'] = 'value'
        cursor.close()

        conn_reconfig = 'debug_mode=(cursor_copy=false)'
        self.conn.reconfigure(conn_reconfig)
        cursor = self.session.open_cursor(self.uri, None)
        cursor['key'] = 'value'
        cursor.close()

        self.conn.reconfigure(self.conn_config)
        cursor = self.session.open_cursor(self.uri, None)
        cursor['key'] = 'value'
        cursor.close()

if __name__ == '__main__':
    wttest.run()
