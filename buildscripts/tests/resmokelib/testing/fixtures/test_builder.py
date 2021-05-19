"""Unit tests for the resmokelib.testing.fixtures.multiversion module."""

import unittest
import filecmp
import os

from buildscripts.resmokelib.testing.fixtures import _builder

TEST_COMMIT = "9fbf58e9f1bee720d270cfa9621f581a0212e5ff"


class TestFixtureBuilder(unittest.TestCase):
    """Class that test retrieve_fixtures methods."""

    def test_retrieve_fixtures(self):
        """function to test retrieve_fixtures"""
        dirpath = os.path.join("build", "multiversionfixture")
        expected_standalone = os.path.join("buildscripts", "tests", "resmokelib", "testing",
                                           "fixtures", "retrieved_fixture.txt")
        _builder.retrieve_fixtures(dirpath, TEST_COMMIT)
        retrieved_standalone = os.path.join(dirpath, "standalone.py")
        self.assertTrue(
            filecmp.cmpfiles(retrieved_standalone, expected_standalone,
                             ["standalone.py", "retrieved_fixture.txt"], shallow=False))
