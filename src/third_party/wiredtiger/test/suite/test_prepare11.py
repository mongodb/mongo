#!/usr/bin/env python
#
# Public Domain 2014-2020 MongoDB, Inc.
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

import wiredtiger, wttest
def timestamp_str(t):
    return '%x' % t

# test_prepare11.py
# Test prepare rollback with a reserved update between updates.
class test_prepare11(wttest.WiredTigerTestCase):
    conn_config = 'cache_size=2MB,statistics=(all)'
    session_config = 'isolation=snapshot'

    def test_prepare_update_rollback(self):
        uri = "table:test_prepare11"
        self.session.create(uri, 'key_format=S,value_format=S')
        self.session.begin_transaction("isolation=snapshot")

        # In the scenario where we have a reserved update in between two updates, the key repeated
        # flag won't get set and we'll call resolve prepared op on both prepared updates.
        c = self.session.open_cursor(uri, None)
        c['key1'] = 'xxxx'
        c.set_key('key1')
        c.reserve()
        c['key1'] = 'yyyy'
        self.session.prepare_transaction('prepare_timestamp=10')
        self.session.rollback_transaction()
