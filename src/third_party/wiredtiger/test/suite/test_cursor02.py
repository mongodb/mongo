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

import wiredtiger
from test_cursor_tracker import TestCursorTracker
from wtscenario import make_scenarios

# test_cursor02.py
#     Cursor operations on small tables.
class test_cursor02(TestCursorTracker):
    """
    Cursor operations on small tables of each access method.
    We use the TestCursorTracker base class to generate
    key/value content and to track/verify content
    after inserts and removes.
    """
    scenarios = make_scenarios([
        ('row', dict(tablekind='row', uri='table')),
        ('lsm-row', dict(tablekind='row', uri='lsm')),
        ('col', dict(tablekind='col', uri='table')),
        ('fix', dict(tablekind='fix', uri='table'))
    ])

    def create_session_and_cursor(self, ninitialentries):
        tablearg = self.uri + ":" + self.table_name1
        if self.tablekind == 'row':
            keyformat = 'key_format=S'
        else:
            keyformat = 'key_format=r'  # record format
        if self.tablekind == 'fix':
            valformat = 'value_format=8t'
        else:
            valformat = 'value_format=S'
        create_args = keyformat + ',' + valformat + self.config_string()
        self.session_create(tablearg, create_args)
        self.pr('creating cursor')
        self.cur_initial_conditions(self.table_name1, ninitialentries, self.tablekind, None, None, self.uri)
        return self.session.open_cursor(tablearg, None, 'append')

    def test_multiple_remove(self):
        """
        Test multiple deletes at the same place
        """
        cursor = self.create_session_and_cursor(10)
        self.cur_first(cursor)
        self.cur_check_forward(cursor, 5)

        self.cur_remove_here(cursor)
        self.cur_next(cursor)
        self.cur_check_here(cursor)

        self.cur_remove_here(cursor)
        self.cur_next(cursor)
        self.cur_check_here(cursor)
        self.cur_check_forward(cursor, 2)

        #self.cur_dump_here(cursor, 'after second next: ')
        cursor.close()

    def test_insert_and_remove(self):
        """
        Test a variety of insert, deletes and iteration
        """
        cursor = self.create_session_and_cursor(10)
        #self.table_dump(self.table_name1)
        self.cur_insert(cursor, 2, 5)
        self.cur_insert(cursor, 2, 6)
        self.cur_insert(cursor, 2, 4)
        self.cur_insert(cursor, 2, 7)
        self.cur_first(cursor)
        self.cur_check_forward(cursor, 5)
        self.cur_remove_here(cursor)
        self.cur_check_forward(cursor, 5)
        self.cur_remove_here(cursor)
        self.cur_check_forward(cursor, 1)
        self.cur_remove_here(cursor)
        self.cur_check_forward(cursor, 2)
        self.cur_check_backward(cursor, 4)
        self.cur_remove_here(cursor)
        self.cur_check_backward(cursor, 2)
        self.cur_check_forward(cursor, 5)
        self.cur_check_backward(cursor, -1)
        self.cur_check_forward(cursor, -1)
        self.cur_last(cursor)
        self.cur_check_backward(cursor, -1)
        cursor.close()

    def test_iterate_empty(self):
        """
        Test iterating an empty table
        """
        cursor = self.create_session_and_cursor(0)
        self.cur_first(cursor, wiredtiger.WT_NOTFOUND)
        self.cur_check_forward(cursor, -1)
        self.cur_check_backward(cursor, -1)
        self.cur_last(cursor, wiredtiger.WT_NOTFOUND)
        self.cur_check_backward(cursor, -1)
        self.cur_check_forward(cursor, -1)
        cursor.close()

    def test_iterate_one_preexisting(self):
        """
        Test iterating a table that contains one preexisting entry
        """
        cursor = self.create_session_and_cursor(1)
        self.cur_first(cursor)
        self.cur_check_forward(cursor, -1)
        self.cur_check_backward(cursor, -1)
        self.cur_last(cursor)
        self.cur_check_backward(cursor, -1)
        self.cur_check_forward(cursor, -1)
        cursor.close()

    def test_iterate_one_added(self):
        """
        Test iterating a table that contains one newly added entry
        """
        cursor = self.create_session_and_cursor(0)
        self.cur_insert(cursor, 1, 0)
        self.cur_first(cursor)
        self.cur_check_forward(cursor, -1)
        self.cur_check_backward(cursor, -1)
        self.cur_last(cursor)
        self.cur_check_backward(cursor, -1)
        self.cur_check_forward(cursor, -1)
        cursor.close()

if __name__ == '__main__':
    wttest.run()
