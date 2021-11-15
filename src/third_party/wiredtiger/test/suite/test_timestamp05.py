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

class test_timestamp05(wttest.WiredTigerTestCase, suite_subprocess):
    uri = 'table:ts05'
    session_config = 'isolation=snapshot'

    format_values = [
        ('integer-row', dict(key_format='i', value_format='S')),
        ('column', dict(key_format='r', value_format='S')),
        ('column-fix', dict(key_format='r', value_format='8t')),
    ]
    scenarios = make_scenarios(format_values)

    def test_create(self):
        s = self.session
        conn = self.conn

        new_value = 71 if self.value_format == '8t' else 'new value'

        # Start timestamps at 50
        conn.set_timestamp('oldest_timestamp=50,stable_timestamp=50')

        # Commit at 100
        s.begin_transaction()
        s.create(self.uri, 'key_format={},value_format={}'.format(self.key_format, self.value_format))
        s.commit_transaction('commit_timestamp=' + self.timestamp_str(100))

        # Make sure the tree is dirty
        c = s.open_cursor(self.uri)
        c[200] = new_value

        # Checkpoint at 50
        s.checkpoint('use_timestamp=true')

    def test_bulk(self):
        s = self.session
        conn = self.conn

        some_value = 66 if self.value_format == '8t' else 'some value'
        new_value = 71 if self.value_format == '8t' else 'new value'

        s.create(self.uri, 'key_format={},value_format={}'.format(self.key_format, self.value_format))
        c = s.open_cursor(self.uri, None, 'bulk')

        # Insert keys 1..100 each with timestamp=key, in some order
        nkeys = 100
        for k in range(1, nkeys+1):
            c[k] = some_value

        # Start timestamps at 50
        conn.set_timestamp('oldest_timestamp=50,stable_timestamp=50')

        # Commit at 100
        s.begin_transaction()
        c.close()
        s.commit_transaction('commit_timestamp=' + self.timestamp_str(100))

        # Make sure the tree is dirty
        c = s.open_cursor(self.uri)
        c[200] = new_value

        # Checkpoint at 50
        s.checkpoint('use_timestamp=true')

if __name__ == '__main__':
    wttest.run()
