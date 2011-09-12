#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_cursor01.py
# 	Cursor operations
#

import unittest
import wiredtiger
from test_cursor_tracker import TestCursorTracker

class test_cursor02(TestCursorTracker):
    def test_multiple_remove(self):
        """
        Test multiple deletes at the same place
        """
        create_args = 'key_format=S,value_format=S' + self.config_string()
        self.session_create("table:" + self.table_name1, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1, None, None)
        self.cur_initial_conditions(cursor, 10, 10, 0)
        self.cur_first(cursor)
        self.cur_check_forward(cursor, 5)
        self.cur_dump_here(cursor, 'before first remove: ')
        self.cur_remove_here(cursor)
        self.cur_dump_here(cursor, 'before second remove: ')
        self.cur_remove_here(cursor)
        self.cur_dump_here(cursor, 'after second remove: ')
        cursor.close(None)

    def test_insert_and_remove(self):
        create_args = 'key_format=S,value_format=S' + self.config_string()
        self.session_create("table:" + self.table_name1, create_args)
        self.pr('creating cursor')
        cursor = self.session.open_cursor('table:' + self.table_name1, None, None)
        self.cur_initial_conditions(cursor, 10, 10, 0)
        self.table_dump(self.table_name1)
        self.cur_insert(cursor, 2, 5)
        self.cur_insert(cursor, 2, 6)
        self.cur_insert(cursor, 2, 4)
        self.cur_insert(cursor, 2, 7)
        self.cur_first(cursor)
        self.cur_check_forward(cursor, 5)
        
        # TODO: add and modify this test when double removal works
        #      and removal is well defined.
        # when a key is removed, do we get positioned
        # on the next key?  or 'between keys', so that
        # we have to do a 'cursor->next' to have a valid
        # pos?  or something else equiv to 'cursor->reset()'?

        # self.cur_remove_here(cursor)
        # self.cur_check_forward(cursor, 5)
        # self.cur_dump_here(cursor, 'before first remove: ')
        # self.cur_remove_here(cursor)
        # self.cur_dump_here(cursor, 'before second remove: ')
        # self.cur_remove_here(cursor)
        # self.cur_dump_here(cursor, 'after second remove: ')
        # self.cur_check_forward(cursor, 2)
        #
        # self.cur_check_backwards(cursor, 10)
        # self.cur_remove_here(cursor)
        # self.cur_check_backwards(cursor, 2)
        # self.cur_check_forward(cursor, 5)
        # self.cur_check_backwards(cursor, -1)
        # self.dumpbitlist()
        # self.cur_check_forward(cursor, -1)

        cursor.close(None)

if __name__ == '__main__':
    wttest.run()
