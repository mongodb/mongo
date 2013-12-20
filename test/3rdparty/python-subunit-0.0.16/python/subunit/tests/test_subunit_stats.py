#
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2005  Robert Collins <robertc@robertcollins.net>
#
#  Licensed under either the Apache License, Version 2.0 or the BSD 3-clause
#  license at the users choice. A copy of both licenses are available in the
#  project source as Apache-2.0 and BSD. You may not use this file except in
#  compliance with one of these two licences.
#  
#  Unless required by applicable law or agreed to in writing, software
#  distributed under these licenses is distributed on an "AS IS" BASIS, WITHOUT
#  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
#  license you chose for the specific language governing permissions and
#  limitations under that license.
#

"""Tests for subunit.TestResultStats."""

import unittest

from testtools.compat import _b, BytesIO, StringIO

import subunit


class TestTestResultStats(unittest.TestCase):
    """Test for TestResultStats, a TestResult object that generates stats."""

    def setUp(self):
        self.output = StringIO()
        self.result = subunit.TestResultStats(self.output)
        self.input_stream = BytesIO()
        self.test = subunit.ProtocolTestCase(self.input_stream)

    def test_stats_empty(self):
        self.test.run(self.result)
        self.assertEqual(0, self.result.total_tests)
        self.assertEqual(0, self.result.passed_tests)
        self.assertEqual(0, self.result.failed_tests)
        self.assertEqual(set(), self.result.seen_tags)

    def setUpUsedStream(self):
        self.input_stream.write(_b("""tags: global
test passed
success passed
test failed
tags: local
failure failed
test error
error error
test skipped
skip skipped
test todo
xfail todo
"""))
        self.input_stream.seek(0)
        self.test.run(self.result)
    
    def test_stats_smoke_everything(self):
        # Statistics are calculated usefully.
        self.setUpUsedStream()
        self.assertEqual(5, self.result.total_tests)
        self.assertEqual(2, self.result.passed_tests)
        self.assertEqual(2, self.result.failed_tests)
        self.assertEqual(1, self.result.skipped_tests)
        self.assertEqual(set(["global", "local"]), self.result.seen_tags)

    def test_stat_formatting(self):
        expected = ("""
Total tests:       5
Passed tests:      2
Failed tests:      2
Skipped tests:     1
Seen tags: global, local
""")[1:]
        self.setUpUsedStream()
        self.result.formatStats()
        self.assertEqual(expected, self.output.getvalue())
