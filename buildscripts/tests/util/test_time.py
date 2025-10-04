"""Unit test for the util.time module."""

import unittest

from buildscripts.util import time as time_utils


class Ns2SecTest(unittest.TestCase):
    def test_ns_converted_to_seconds(self):
        self.assertEqual(time_utils.ns2sec(10**9), 1)
