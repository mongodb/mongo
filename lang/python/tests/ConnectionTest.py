#!/usr/bin/env python
#
# Copyright (c) 2010 WiredTiger.  All rights reserved.
#
# Python unit tests for the wiredtiger.Connection class.

from wiredtiger import Connection

try:
    import unittest2 as unittest
except ImportError:
    import unittest

class TestConnections(unittest.TestCase):
    def setUp(self):
        pass

    def test_open(self):
        c = Connection('file:/testdb')

if __name__ == '__main__':
    unittest.main()
