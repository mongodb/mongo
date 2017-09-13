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

"""Tests for TAP2SubUnit."""

from io import BytesIO, StringIO
import unittest

from testtools import TestCase
from testtools.compat import _u
from testtools.testresult.doubles import StreamResult

import subunit

UTF8_TEXT = 'text/plain; charset=UTF8'


class TestTAP2SubUnit(TestCase):
    """Tests for TAP2SubUnit.

    These tests test TAP string data in, and subunit string data out.
    This is ok because the subunit protocol is intended to be stable,
    but it might be easier/pithier to write tests against TAP string in,
    parsed subunit objects out (by hooking the subunit stream to a subunit
    protocol server.
    """

    def setUp(self):
        super(TestTAP2SubUnit, self).setUp()
        self.tap = StringIO()
        self.subunit = BytesIO()

    def test_skip_entire_file(self):
        # A file
        # 1..- # Skipped: comment
        # results in a single skipped test.
        self.tap.write(_u("1..0 # Skipped: entire file skipped\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'file skip', 'skip', None, True,
            'tap comment', b'Skipped: entire file skipped', True, None, None,
            None)])

    def test_ok_test_pass(self):
        # A file
        # ok
        # results in a passed test with name 'test 1' (a synthetic name as tap
        # does not require named fixtures - it is the first test in the tap
        # stream).
        self.tap.write(_u("ok\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1', 'success', None, False, None,
            None, True, None, None, None)])

    def test_ok_test_number_pass(self):
        # A file
        # ok 1
        # results in a passed test with name 'test 1'
        self.tap.write(_u("ok 1\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1', 'success', None, False, None,
            None, True, None, None, None)])

    def test_ok_test_number_description_pass(self):
        # A file
        # ok 1 - There is a description
        # results in a passed test with name 'test 1 - There is a description'
        self.tap.write(_u("ok 1 - There is a description\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1 - There is a description',
            'success', None, False, None, None, True, None, None, None)])

    def test_ok_test_description_pass(self):
        # A file
        # ok There is a description
        # results in a passed test with name 'test 1 There is a description'
        self.tap.write(_u("ok There is a description\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1 There is a description',
            'success', None, False, None, None, True, None, None, None)])

    def test_ok_SKIP_skip(self):
        # A file
        # ok # SKIP
        # results in a skkip test with name 'test 1'
        self.tap.write(_u("ok # SKIP\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1', 'skip', None, False, None,
            None, True, None, None, None)])

    def test_ok_skip_number_comment_lowercase(self):
        self.tap.write(_u("ok 1 # skip no samba environment available, skipping compilation\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1', 'skip', None, False, 'tap comment',
            b'no samba environment available, skipping compilation', True,
            'text/plain; charset=UTF8', None, None)])

    def test_ok_number_description_SKIP_skip_comment(self):
        # A file
        # ok 1 foo  # SKIP Not done yet
        # results in a skip test with name 'test 1 foo' and a log of
        # Not done yet
        self.tap.write(_u("ok 1 foo  # SKIP Not done yet\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1 foo', 'skip', None, False,
            'tap comment', b'Not done yet', True, 'text/plain; charset=UTF8',
            None, None)])

    def test_ok_SKIP_skip_comment(self):
        # A file
        # ok # SKIP Not done yet
        # results in a skip test with name 'test 1' and a log of Not done yet
        self.tap.write(_u("ok # SKIP Not done yet\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1', 'skip', None, False,
            'tap comment', b'Not done yet', True, 'text/plain; charset=UTF8',
            None, None)])

    def test_ok_TODO_xfail(self):
        # A file
        # ok # TODO
        # results in a xfail test with name 'test 1'
        self.tap.write(_u("ok # TODO\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1', 'xfail', None, False, None,
            None, True, None, None, None)])

    def test_ok_TODO_xfail_comment(self):
        # A file
        # ok # TODO Not done yet
        # results in a xfail test with name 'test 1' and a log of Not done yet
        self.tap.write(_u("ok # TODO Not done yet\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([('status', 'test 1', 'xfail', None, False,
            'tap comment', b'Not done yet', True, 'text/plain; charset=UTF8',
            None, None)])

    def test_bail_out_errors(self):
        # A file with line in it
        # Bail out! COMMENT
        # is treated as an error
        self.tap.write(_u("ok 1 foo\n"))
        self.tap.write(_u("Bail out! Lifejacket engaged\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1 foo', 'success', None, False, None, None, True,
             None, None, None),
            ('status', 'Bail out! Lifejacket engaged', 'fail', None, False,
             None, None, True, None, None, None)])

    def test_missing_test_at_end_with_plan_adds_error(self):
        # A file
        # 1..3
        # ok first test
        # not ok third test
        # results in three tests, with the third being created
        self.tap.write(_u('1..3\n'))
        self.tap.write(_u('ok first test\n'))
        self.tap.write(_u('not ok second test\n'))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1 first test', 'success', None, False, None,
             None, True, None, None, None),
            ('status', 'test 2 second test', 'fail', None, False, None, None,
             True, None, None, None),
            ('status', 'test 3', 'fail', None, False, 'tap meta',
             b'test missing from TAP output', True, 'text/plain; charset=UTF8',
             None, None)])

    def test_missing_test_with_plan_adds_error(self):
        # A file
        # 1..3
        # ok first test
        # not ok 3 third test
        # results in three tests, with the second being created
        self.tap.write(_u('1..3\n'))
        self.tap.write(_u('ok first test\n'))
        self.tap.write(_u('not ok 3 third test\n'))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1 first test', 'success', None, False, None, None,
             True, None, None, None),
            ('status', 'test 2', 'fail', None, False, 'tap meta',
             b'test missing from TAP output', True, 'text/plain; charset=UTF8',
             None, None),
            ('status', 'test 3 third test', 'fail', None, False, None, None,
             True, None, None, None)])

    def test_missing_test_no_plan_adds_error(self):
        # A file
        # ok first test
        # not ok 3 third test
        # results in three tests, with the second being created
        self.tap.write(_u('ok first test\n'))
        self.tap.write(_u('not ok 3 third test\n'))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1 first test', 'success', None, False, None, None,
             True, None, None, None),
            ('status', 'test 2', 'fail', None, False, 'tap meta',
             b'test missing from TAP output', True, 'text/plain; charset=UTF8',
             None, None),
            ('status', 'test 3 third test', 'fail', None, False, None, None,
             True, None, None, None)])

    def test_four_tests_in_a_row_trailing_plan(self):
        # A file
        # ok 1 - first test in a script with no plan at all
        # not ok 2 - second
        # ok 3 - third
        # not ok 4 - fourth
        # 1..4
        # results in four tests numbered and named
        self.tap.write(_u('ok 1 - first test in a script with trailing plan\n'))
        self.tap.write(_u('not ok 2 - second\n'))
        self.tap.write(_u('ok 3 - third\n'))
        self.tap.write(_u('not ok 4 - fourth\n'))
        self.tap.write(_u('1..4\n'))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1 - first test in a script with trailing plan',
             'success', None, False, None, None, True, None, None, None),
            ('status', 'test 2 - second', 'fail', None, False, None, None,
             True, None, None, None),
            ('status', 'test 3 - third', 'success', None, False, None, None,
             True, None, None, None),
            ('status', 'test 4 - fourth', 'fail', None, False, None, None,
             True, None, None, None)])

    def test_four_tests_in_a_row_with_plan(self):
        # A file
        # 1..4
        # ok 1 - first test in a script with no plan at all
        # not ok 2 - second
        # ok 3 - third
        # not ok 4 - fourth
        # results in four tests numbered and named
        self.tap.write(_u('1..4\n'))
        self.tap.write(_u('ok 1 - first test in a script with a plan\n'))
        self.tap.write(_u('not ok 2 - second\n'))
        self.tap.write(_u('ok 3 - third\n'))
        self.tap.write(_u('not ok 4 - fourth\n'))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1 - first test in a script with a plan',
             'success', None, False, None, None, True, None, None, None),
            ('status', 'test 2 - second', 'fail', None, False, None, None,
             True, None, None, None),
            ('status', 'test 3 - third', 'success', None, False, None, None,
             True, None, None, None),
            ('status', 'test 4 - fourth', 'fail', None, False, None, None,
             True, None, None, None)])

    def test_four_tests_in_a_row_no_plan(self):
        # A file
        # ok 1 - first test in a script with no plan at all
        # not ok 2 - second
        # ok 3 - third
        # not ok 4 - fourth
        # results in four tests numbered and named
        self.tap.write(_u('ok 1 - first test in a script with no plan at all\n'))
        self.tap.write(_u('not ok 2 - second\n'))
        self.tap.write(_u('ok 3 - third\n'))
        self.tap.write(_u('not ok 4 - fourth\n'))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1 - first test in a script with no plan at all',
             'success', None, False, None, None, True, None, None, None),
            ('status', 'test 2 - second', 'fail', None, False, None, None,
             True, None, None, None),
            ('status', 'test 3 - third', 'success', None, False, None, None,
             True, None, None, None),
            ('status', 'test 4 - fourth', 'fail', None, False, None, None,
             True, None, None, None)])

    def test_todo_and_skip(self):
        # A file
        # not ok 1 - a fail but # TODO but is TODO
        # not ok 2 - another fail # SKIP instead
        # results in two tests, numbered and commented.
        self.tap.write(_u("not ok 1 - a fail but # TODO but is TODO\n"))
        self.tap.write(_u("not ok 2 - another fail # SKIP instead\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.subunit.seek(0)
        events = StreamResult()
        subunit.ByteStreamToStreamResult(self.subunit).run(events)
        self.check_events([
            ('status', 'test 1 - a fail but', 'xfail', None, False,
             'tap comment', b'but is TODO', True, 'text/plain; charset=UTF8',
             None, None),
            ('status', 'test 2 - another fail', 'skip', None, False,
             'tap comment', b'instead', True, 'text/plain; charset=UTF8',
             None, None)])

    def test_leading_comments_add_to_next_test_log(self):
        # A file
        # # comment
        # ok 
        # ok
        # results in a single test with the comment included
        # in the first test and not the second.
        self.tap.write(_u("# comment\n"))
        self.tap.write(_u("ok\n"))
        self.tap.write(_u("ok\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1', 'success', None, False, 'tap comment',
             b'# comment', True, 'text/plain; charset=UTF8', None, None),
            ('status', 'test 2', 'success', None, False, None, None, True,
             None, None, None)])
    
    def test_trailing_comments_are_included_in_last_test_log(self):
        # A file
        # ok foo
        # ok foo
        # # comment
        # results in a two tests, with the second having the comment
        # attached to its log.
        self.tap.write(_u("ok\n"))
        self.tap.write(_u("ok\n"))
        self.tap.write(_u("# comment\n"))
        self.tap.seek(0)
        result = subunit.TAP2SubUnit(self.tap, self.subunit)
        self.assertEqual(0, result)
        self.check_events([
            ('status', 'test 1', 'success', None, False, None, None, True,
             None, None, None),
            ('status', 'test 2', 'success', None, False, 'tap comment',
             b'# comment', True, 'text/plain; charset=UTF8', None, None)])

    def check_events(self, events):
        self.subunit.seek(0)
        eventstream = StreamResult()
        subunit.ByteStreamToStreamResult(self.subunit).run(eventstream)
        self.assertEqual(events, eventstream._events)
