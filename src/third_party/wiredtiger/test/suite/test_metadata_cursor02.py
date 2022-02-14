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
from wtscenario import make_scenarios

# test_metadata_cursor02.py
#    Metadata cursor operations with invalid metadata
#
# Test metadata cursor semantics when the underlying metadata is invalid.
# This can happen after a crash, or if part of a table is dropped separate
# from dropping the whole table.
class test_metadata_cursor02(wttest.WiredTigerTestCase):
    """
    Test metadata cursor operations with invalid metadata
    """
    table_name1 = 'table:t1'
    table_name2 = 'table:t2'
    table_name3 = 'table:t3'
    tables = [table_name1, table_name2, table_name3]

    scenarios = make_scenarios([
        ('plain', {'metauri' : 'metadata:'}),
        ('create', {'metauri' : 'metadata:create'}),
    ], [
        ('drop_colgroup', {'drop' : 'colgroup'}),
        ('drop_file', {'drop' : 'file'}),
    ])

    # Create tables
    def create_tables(self):
        # Reopen to make sure we can drop anything left over from the last run
        self.reopen_conn()
        for name in self.tables:
            self.session.drop(name, 'force=true')
            self.session.create(name, 'key_format=S,value_format=S')

    # Forward iteration.
    def test_missing(self):
        for name in self.tables:
            self.create_tables()

            # Invalidate the table by dropping part of it
            if self.drop == 'colgroup':
                self.session.drop('colgroup:' + name[-2:])
            else:
                self.session.drop('file:' + name[-2:] + '.wt')

            cursor = self.session.open_cursor(self.metauri)
            is_create_cursor = self.metauri.endswith('create')
            count = 0
            for k, v in cursor:
                self.pr('Found metadata entry: ' + k)
                if k.startswith('table:'):
                    count += 1
            cursor.close()

            if is_create_cursor:
                self.captureerr.checkAdditionalPattern(self, 'metadata information.*not found')

            # Should include the metadata and the two valid tables
            self.assertEqual(count, self.metauri.endswith('create') and 2 or 3)

if __name__ == '__main__':
    wttest.run()
