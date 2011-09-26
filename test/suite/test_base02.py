#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_base02.py
# 	Configuration
#

#### This test has workarounds to allow it to complete, marked with '####' comments

import unittest
import wiredtiger
import wttest
import json

class test_base02(wttest.WiredTigerTestCase):
    """
    Test configuration strings
    """
    table_name1 = 'test_base02a'

    def create_and_drop_table(self, tablename, confstr):
        self.pr('create_table with config:\n      ' + confstr)
        self.session.create('table:' + tablename, confstr)
        self.session.drop('table:' + tablename, None)

    def test_config_combinations(self):
        """
        Spot check various combinations of configuration options.
        """
        conf_confsize = [
            None,
            'allocation_size=1024',
            'internal_node_max=64k,internal_overflow_size=1k',
            'leaf_node_max=128k,leaf_overflow_size=512',
            'leaf_node_max=256k,leaf_overflow_size=256,internal_node_max=8k,internal_overflow_size=128',
            ]
        conf_col = [
            'columns=(first,second, third)',
            'columns=(first)',
            'key_format="5S", value_format="Su", columns=(first,second, third)',
            ',,columns=(first=S,second="4u", third=S),,',
            ]
        conf_encoding = [
            None,
            'huffman_key=,huffman_value=english',
            ]
        for size in conf_confsize:
            for col in conf_col:
                for enc in conf_encoding:
                    conflist = [size, col, enc]
                    confstr = ",".join([c for c in conflist if c != None])
                    self.create_and_drop_table(self.table_name1, confstr)

    def test_config_json(self):
        """
        Spot check various combinations of configuration options, using JSON format.
        """
        conf_jsonstr = [
            json.dumps({'columns' : ('one', 'two', 'three')}),
            json.dumps({
                    "key_format" : "r",
                    "value_format" : "5sHQ",
                    "columns" : ("id", "country", "year", "population"),
                    "colgroups" : ("cyear", "population"),
			})]
        for confstr in conf_jsonstr:
            self.create_and_drop_table(self.table_name1, confstr)

if __name__ == '__main__':
    wttest.run()
