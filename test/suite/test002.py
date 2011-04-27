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

import unittest
import wiredtiger
import wttest
import json

####import config

class test002(wttest.WiredTigerTestCase):
    """
    Test configuration strings
    """
    table_name1 = 'test002a.wt'

    def create_and_drop_table(self, tablename, confstr):
        self.pr('create_table with config: ' + confstr)
        self.session.create_table(tablename, confstr)
        self.session.drop_table(tablename, None)

    def test_config_combinations(self):
        conf_create = [
            'create',
            'create,cachesize=10MB',
            'create,cachesize=10MB,path="/foo/bar"']
        conf_col = [
            'columns=(first,second, third)',
            'key_format="S", value_format="5sq", columns=(first,second, third)',
            'key_columns=(first=S),value_columns=(second="5s", third=q)',
            ',,columns=(first=S,second="5s", third=q),,',
            'index.country_year=(country,year),key_format=r,colgroup.population=(population),columns=(id,country,year,population),value_format=5sHQ']
        for create in conf_create:
            for col in conf_col:
                confstr = ",".join([create, col])
                self.create_and_drop_table(self.table_name1, confstr)

    def test_config_json(self):
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
    wttest.run(test002)
