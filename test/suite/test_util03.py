#!/usr/bin/env python
#
# See the file LICENSE for redistribution information.
#
# Copyright (c) 2008-2011 WiredTiger, Inc.
#	All rights reserved.
#
# test_util03.py
# 	Utilities: wt create
#

import unittest
from wiredtiger import WiredTigerError
import wttest
from suite_subprocess import suite_subprocess

class test_util03(wttest.WiredTigerTestCase, suite_subprocess):
    tablename = 'test_util03.a'
    nentries = 1000

    scenarios = [
        ('none', dict(key_format=None,value_format=None)),
        ('SS', dict(key_format='S',value_format='S')),
        ('rS', dict(key_format='r',value_format='S')),
        ('ri', dict(key_format='r',value_format='i')),
        ]

    def test_create_process(self):
        """
        Test create in a 'wt' process
        """

        args = ["create"]
        if self.key_format != None or self.value_format != None:
            args.append('-c')
            config = ''
            if self.key_format != None:
                config += 'key_format=' + self.key_format + ','
            if self.value_format != None:
                config += 'value_format=' + self.value_format
            args.append(config)
        args.append('table:' + self.tablename)
        self.runWt(args)

        cursor = self.session.open_cursor('table:' + self.tablename, None, None)
        if self.key_format != None:
            self.assertEqual(cursor.key_format, self.key_format)
        if self.value_format != None:
            self.assertEqual(cursor.value_format, self.value_format)
        for key,val in cursor:
            self.fail('table should be empty')
        cursor.close()


if __name__ == '__main__':
    wttest.run()
