#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_cursor02.py
# 	Cursor operations on tables of various sizes,
#       with key/values of various sizes.
#

import unittest
import wiredtiger
from test_cursor_tracker import TestCursorTracker
from wtscenario import multiply_scenarios

class test_cursor03(TestCursorTracker):
    """
    Cursor operations on small tables of each access method.
    We use the TestCursorTracker base class to generate
    key/value content and to track/verify content
    after inserts and removes.
    """
    TODO_scenarios = multiply_scenarios('.', [
            ('row', dict(tablekind='row', keysize=None, valsize=None)),
            ('col', dict(tablekind='col', keysize=None, valsize=None)),
            #('fix', dict(tablekind='fix', keysize=None, valsize=None))
            ('row.val10k', dict(tablekind='row', keysize=None, valsize=[10, 10000])),
            ('col.val10k', dict(tablekind='col', keysize=None, valsize=[10, 10000])),
            ('row.keyval10k', dict(tablekind='row', keysize=[10,10000], valsize=[10, 10000])),
        ], [
            ('count1000', dict(tablecount=1000)),
            ('count100000', dict(tablecount=100000))
            ])

    # TODO: use the scenarios above.  For now we have a single fixed scenario:
    scenarios = [('count100000.col', dict(tablekind='col', tablecount=100000, keysize=None, valsize=None))]

    def create_session_and_cursor(self):
        tablearg = "table:" + self.table_name1
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
        self.cur_initial_conditions(self.table_name1, self.tablecount, self.tablekind)
        return self.session.open_cursor(tablearg, None, None)

    def test_multiple_remove(self):
        """
        Test multiple deletes at the same place
        """
        cursor = self.create_session_and_cursor()
        self.cur_first(cursor)
        self.cur_check_forward(cursor, 5)

        self.cur_remove_here(cursor)
        self.cur_next(cursor)
        self.cur_check_here(cursor)

        self.cur_remove_here(cursor)
        self.cur_next(cursor)
        self.cur_check_here(cursor)
        self.cur_check_forward(cursor, 2)

        self.cur_dump_here(cursor, 'after second next: ')
        cursor.close(None)

    def test_insert_and_remove(self):
        """
        Test a variety of insert, deletes and iteration
        """
        cursor = self.create_session_and_cursor()
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
        # TODO: print statements added to narrow a bug
        print('>>> before iterate forward to the end...')
        self.cur_check_forward(cursor, -1)
        print('>>> after iterate forward to the end...')
        self.cur_last(cursor)
        self.cur_check_backward(cursor, -1)
        cursor.close(None)

if __name__ == '__main__':
    wttest.run()
