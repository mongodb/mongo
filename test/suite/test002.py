#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test002.py
# 	Configuration
#

#### This test has workarounds to allow it to complete, marked with '####' comments

import unittest
import wiredtiger
import wttest
import json

class test002(wttest.WiredTigerTestCase):
    """
    Test configuration strings
    """
    table_name1 = 'test002a.wt'

    def create_and_drop_table(self, tablename, confstr):
        self.pr('create_table with config:\n      ' + confstr)
        self.session.create('table:' + tablename, confstr)

        #### Drop table not implemented, instead, we're able to explicitly remove the file
        ####self.session.drop('table:' + tablename, None)

        import subprocess                          #### added
        subprocess.call(["rm", "-f", tablename])   #### added

    def test_config_combinations(self):
        """
        Spot check various combinations of configuration options.
        """
        conf_confsize = [
            None,
            'allocation_size=1024',
            'intl_node_max=64k,intl_node_min=4k',
            'leaf_node_max=128k,leaf_node_min=512',
            'leaf_node_max=256k,leaf_node_min=1k,intl_node_max=8k,intl_node_min=512',
            ]
        conf_col = [
            #### Fixed size strings (e.g. '5s') not yet implemented
            'columns=(first,second, third)',
            'columns=(first)',
            'key_format="S", value_format="Su", columns=(first,second, third)',
            ',,columns=(first=S,second="4u", third=S),,',
            #### index.xxxx, colgroup.xxxx not yet implemented
            ####'index.country_year=(country,year),key_format=r,colgroup.population=(population),columns=(id,country,year,population),value_format=SS2u',
            ]
        conf_encoding = [
            None,
            'huffman_key=,huffman_value=english',
            'runlength_encoding'
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
                    "colgroup.population" : ("population",),
                    "index.country_year" : ("country","year")})]
        for confstr in conf_jsonstr:
            self.create_and_drop_table(self.table_name1, confstr)


if __name__ == '__main__':
    wttest.run()
