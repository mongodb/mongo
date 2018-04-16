"""Unit test for the util.time module."""

from __future__ import absolute_import

import unittest

import util.time as time_utils

#pylint: disable=missing-docstring


class Ns2SecTest(unittest.TestCase):
    def test_ns_converted_to_seconds(self):
        self.assertEqual(time_utils.ns2sec(10**9), 1)
