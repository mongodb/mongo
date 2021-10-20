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
# test_timestamp05.py
#   Timestamps: make sure they don't end up in metadata
#

import random
from suite_subprocess import suite_subprocess
import wiredtiger, wttest
from wtscenario import make_scenarios

def timestamp_str(t):
    return '%x' % t

class test_timestamp05(wttest.WiredTigerTestCase, suite_subprocess):
    uri = 'table:ts05'

    def test_create(self):
        s = self.session
        conn = self.conn

        # Start timestamps at 50
        conn.set_timestamp('oldest_timestamp=50,stable_timestamp=50')

        # Commit at 100
        s.begin_transaction()
        s.create(self.uri, 'key_format=i,value_format=S')
        s.commit_transaction('commit_timestamp=' + timestamp_str(100))

        # Make sure the tree is dirty
        c = s.open_cursor(self.uri)
        c[200] = 'new value'

        # Checkpoint at 50
        s.checkpoint('use_timestamp=true')

    def test_bulk(self):
        s = self.session
        conn = self.conn

        s.create(self.uri, 'key_format=i,value_format=S')
        c = s.open_cursor(self.uri, None, 'bulk')

        # Insert keys 1..100 each with timestamp=key, in some order
        nkeys = 100
        keys = range(1, nkeys+1)

        for k in keys:
            c[k] = 'some value'

        # Start timestamps at 50
        conn.set_timestamp('oldest_timestamp=50,stable_timestamp=50')

        # Commit at 100
        s.begin_transaction()
        c.close()
        s.commit_transaction('commit_timestamp=' + timestamp_str(100))

        # Make sure the tree is dirty
        c = s.open_cursor(self.uri)
        c[200] = 'new value'

        # Checkpoint at 50
        s.checkpoint('use_timestamp=true')

if __name__ == '__main__':
    wttest.run()
