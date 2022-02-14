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
# test_txn24.py
#   Test the write generation mechanism to ensure that transaction ids get wiped between runs.
#

import wttest
from wtscenario import make_scenarios

class test_txn25(wttest.WiredTigerTestCase):
    base_config = 'create,cache_size=50MB'
    format_values = [
        ('fix', dict(key_format='r', usestrings=False, value_format='8t')),
        ('row', dict(key_format='S', usestrings=True, value_format='S')),
        ('var', dict(key_format='r', usestrings=False, value_format='S')),
    ]
    log_config = [
        ('logging', dict(conn_config=base_config + ',log=(enabled)')),
        ('no-logging', dict(conn_config=base_config)),
    ]
    scenarios = make_scenarios(format_values, log_config)

    def getkey(self, i):
        return str(i) if self.usestrings else i

    def test_txn25(self):
        uri = 'file:test_txn25'
        create_config = 'key_format={},value_format={}'.format(self.key_format, self.value_format)
        self.session.create(uri, 'allocation_size=512,' + create_config)

        # Populate the file and ensure that we start seeing some high transaction IDs in the system.
        nrows = 1000
        if self.value_format == '8t':
            # Values are 1/500 the size, but for this we don't need to generate a lot of data,
            # just a lot of transactions, so we can keep the same nrows. This will generate only
            # one page, but that shouldn't affect the test criteria.
            value1 = 97
            value2 = 98
            value3 = 99
        else:
            value1 = 'aaaaa' * 100
            value2 = 'bbbbb' * 100
            value3 = 'ccccc' * 100

        # Keep transaction ids around.
        session2 = self.conn.open_session()
        session2.begin_transaction()

        cursor = self.session.open_cursor(uri)
        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor[self.getkey(i)] = value1
            self.session.commit_transaction()

        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor[self.getkey(i)] = value2
            self.session.commit_transaction()

        for i in range(1, nrows):
            self.session.begin_transaction()
            cursor[self.getkey(i)] = value3
            self.session.commit_transaction()

        # Force pages to be written with transaction IDs.
        self.session.checkpoint()

        session2.rollback_transaction()

        # Reopen the connection.
        self.reopen_conn()

        # Now that we've reopened, check that we can view the latest data from the previous run.
        #
        # Since we've restarted the system, our transaction IDs are going to begin from 1 again
        # so we have to wipe the cell's transaction IDs in order to see them.
        cursor = self.session.open_cursor(uri)
        self.session.begin_transaction()
        for i in range(1, nrows):
            self.assertEqual(cursor[self.getkey(i)], value3)
        self.session.rollback_transaction()
