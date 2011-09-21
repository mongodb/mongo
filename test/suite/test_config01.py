#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_base03.py
# 	Cursor operations
#

import unittest
import wiredtiger
import wttest
import test_base03
import wtscenario

class test_config01(test_base03.test_base03):
    scenarios = wtscenario.wtscenario.session_create_scenario()

    def config_string(self):
        return self.session_create_scenario.configString()

    def setUpConnectionOpen(self, dir):
        wtopen_args = 'create'
        if hasattr(self, 'cache_size'):
            wtopen_args += ',cache_size=' + str(self.cache_size)
        conn = wiredtiger.wiredtiger_open(dir, wtopen_args)
        self.pr(`conn`)
        return conn

if __name__ == '__main__':
    wttest.run()
