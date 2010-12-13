#!/usr/bin/env python
#
# Copyright (c) 2010 WiredTiger.  All rights reserved.
#
# Python unit tests for the wiredtiger.*pack* methods.

from wiredtiger import pack, unpack

try:
    import unittest2 as unittest
except ImportError:
    import unittest

class TestPacking(unittest.TestCase):
    def setUp(self):
        pass

    def test_simple(self):
        for b in (0, 10, 65):
            self.assertEqual(pack('b', b), chr(b))
        for b in (0, 10, 65, 255):
            self.assertEqual(pack('B', b), chr(b))
            self.assertEqual(pack('c', chr(b)), chr(b))

if __name__ == '__main__':
    unittest.main()
