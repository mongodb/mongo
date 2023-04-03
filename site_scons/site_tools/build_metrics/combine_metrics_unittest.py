#!/usr/bin/env python3
import sys
import os
import unittest

sys.path.append(os.path.dirname(__file__))

from combine_metrics import set_lowest, set_greatest, combine_command_line, if_set_should_match, recalc_list_indexes, extend_list, extend_list_no_dups


class CombineUnittests(unittest.TestCase):
    def setUp(self):

        self.existing = {
            'int':
                4, 'match_same':
                    'test', 'command_line':
                        'arg1 arg2 dup_arg', 'recalc_list': [{'array_index': 93},
                                                             {'array_index': 3}],
            'extend_list': [{'array_index': 0, 'key': 'text', 'val': 'data1'},
                            {'array_index': 1, 'key': 'text2', 'val': 'data2'}]
        }
        self.current = {
            'int':
                5, 'match_same':
                    'test', 'command_line':
                        'arg3 dup_arg arg4',
            'extend_list': [{'array_index': 0, 'key': 'text', 'val': 'data1'},
                            {'array_index': 1, 'key': 'text3', 'val': 'data3'}]
        }

    def test_set_lowest(self):
        set_lowest(self.existing, self.current, 'int')
        self.assertEqual(self.existing['int'], 4)

    def test_set_greatest(self):
        set_greatest(self.existing, self.current, 'int')
        self.assertEqual(self.existing['int'], 5)

    def test_combine_command_line(self):
        combine_command_line(self.existing, self.current, 'command_line')
        self.assertEqual(self.existing['command_line'], 'arg1 arg2 dup_arg arg3 arg4')

    def test_if_set_should_match(self):
        if_set_should_match(self.existing, self.current, 'match_same')
        del self.current['match_same']
        if_set_should_match(self.existing, self.current, 'match_same')
        self.assertEqual(self.existing['match_same'], 'test')
        self.current['match_same'] = 'test2'
        self.assertRaises(Exception, if_set_should_match, self.existing, self.current, 'match_same')

    def test_recalc_list_indexes(self):
        recalc_list_indexes(self.existing['recalc_list'])
        self.assertEqual(self.existing['recalc_list'], [{'array_index': 0}, {'array_index': 1}])

    def test_extend_list(self):
        extend_list(self.existing, self.current, 'extend_list')
        self.assertEqual(self.existing['extend_list'],
                         [{'array_index': 0, 'key': 'text', 'val': 'data1'},
                          {'array_index': 1, 'key': 'text2', 'val': 'data2'},
                          {'array_index': 2, 'key': 'text', 'val': 'data1'},
                          {'array_index': 3, 'key': 'text3', 'val': 'data3'}])

    def test_extend_list_no_dups(self):
        extend_list_no_dups(self.existing, self.current, 'extend_list', 'key')
        self.assertEqual(self.existing['extend_list'],
                         [{'array_index': 0, 'key': 'text', 'val': 'data1'},
                          {'array_index': 1, 'key': 'text2', 'val': 'data2'},
                          {'array_index': 2, 'key': 'text3', 'val': 'data3'}])

    def test_extend_list_no_dups_bad_data(self):
        if sys.platform != 'win32':
            self.current['extend_list'][0]['val'] = 'bad_data'
            self.assertRaises(Exception, extend_list_no_dups, self.existing, self.current,
                              'extend_list', 'key')


unittest.main()
