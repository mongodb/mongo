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

# test_debug_mode07.py
#   Test the debug mode settings. Test realloc_exact use (from WT-4919).
class test_debug_mode07(wttest.WiredTigerTestCase):
    conn_config = 'log=(enabled=true),debug_mode=(realloc_exact=true)'
    uri = 'file:test_debug_mode07'

    # Insert some data to ensure setting/unsetting the flag does not
    # break existing functionality. Also call checkpoint because it
    # causes the realloc function to be called numerous times.
    def insert_data(self):
        self.session.create(self.uri, 'key_format=s,value_format=s')
        self.cursor = self.session.open_cursor(self.uri, None)
        self.cursor['key'] = 'value'
        self.cursor.close()
        self.session.checkpoint()

    # Make flag works when set.
    def test_realloc_exact(self):
        self.insert_data()

    # Make sure the flag can be 'turned off' as well.
    def test_realloc_exact_off(self):
        conn_reconfig = 'debug_mode=(realloc_exact=false)'
        self.conn.reconfigure(conn_reconfig)
        self.insert_data()

if __name__ == '__main__':
    wttest.run()
