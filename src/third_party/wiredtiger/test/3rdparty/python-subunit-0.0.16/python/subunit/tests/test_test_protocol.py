#
#  subunit: extensions to Python unittest to get test results from subprocesses.
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

import datetime
import unittest
import os

from testtools import PlaceHolder, skipIf, TestCase, TestResult
from testtools.compat import _b, _u, BytesIO
from testtools.content import Content, TracebackContent, text_content
from testtools.content_type import ContentType
try:
    from testtools.testresult.doubles import (
        Python26TestResult,
        Python27TestResult,
        ExtendedTestResult,
        )
except ImportError:
    from testtools.tests.helpers import (
        Python26TestResult,
        Python27TestResult,
        ExtendedTestResult,
        )
from testtools.matchers import Contains

import subunit
from subunit.tests import (
    _remote_exception_repr,
    _remote_exception_str,
    _remote_exception_str_chunked,
    )
import subunit.iso8601 as iso8601


def details_to_str(details):
    return TestResult()._err_details_to_string(None, details=details)


class TestTestImports(unittest.TestCase):

    def test_imports(self):
        from subunit import DiscardStream
        from subunit import TestProtocolServer
        from subunit import RemotedTestCase
        from subunit import RemoteError
        from subunit import ExecTestCase
        from subunit import IsolatedTestCase
        from subunit import TestProtocolClient
        from subunit import ProtocolTestCase


class TestDiscardStream(unittest.TestCase):

    def test_write(self):
        subunit.DiscardStream().write("content")


class TestProtocolServerForward(unittest.TestCase):

    def test_story(self):
        client = unittest.TestResult()
        out = BytesIO()
        protocol = subunit.TestProtocolServer(client, forward_stream=out)
        pipe = BytesIO(_b("test old mcdonald\n"
                        "success old mcdonald\n"))
        protocol.readFrom(pipe)
        self.assertEqual(client.testsRun, 1)
        self.assertEqual(pipe.getvalue(), out.getvalue())

    def test_not_command(self):
        client = unittest.TestResult()
        out = BytesIO()
        protocol = subunit.TestProtocolServer(client,
            stream=subunit.DiscardStream(), forward_stream=out)
        pipe = BytesIO(_b("success old mcdonald\n"))
        protocol.readFrom(pipe)
        self.assertEqual(client.testsRun, 0)
        self.assertEqual(_b(""), out.getvalue())


class TestTestProtocolServerPipe(unittest.TestCase):

    def test_story(self):
        client = unittest.TestResult()
        protocol = subunit.TestProtocolServer(client)
        traceback = "foo.c:53:ERROR invalid state\n"
        pipe = BytesIO(_b("test old mcdonald\n"
                        "success old mcdonald\n"
                        "test bing crosby\n"
                        "failure bing crosby [\n"
                        +  traceback +
                        "]\n"
                        "test an error\n"
                        "error an error\n"))
        protocol.readFrom(pipe)
        bing = subunit.RemotedTestCase("bing crosby")
        an_error = subunit.RemotedTestCase("an error")
        self.assertEqual(client.errors,
                         [(an_error, _remote_exception_repr + '\n')])
        self.assertEqual(
            client.failures,
            [(bing, _remote_exception_repr + ": "
              + details_to_str({'traceback': text_content(traceback)}) + "\n")])
        self.assertEqual(client.testsRun, 3)

    def test_non_test_characters_forwarded_immediately(self):
        pass


class TestTestProtocolServerStartTest(unittest.TestCase):

    def setUp(self):
        self.client = Python26TestResult()
        self.stream = BytesIO()
        self.protocol = subunit.TestProtocolServer(self.client, self.stream)

    def test_start_test(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.assertEqual(self.client._events,
            [('startTest', subunit.RemotedTestCase("old mcdonald"))])

    def test_start_testing(self):
        self.protocol.lineReceived(_b("testing old mcdonald\n"))
        self.assertEqual(self.client._events,
            [('startTest', subunit.RemotedTestCase("old mcdonald"))])

    def test_start_test_colon(self):
        self.protocol.lineReceived(_b("test: old mcdonald\n"))
        self.assertEqual(self.client._events,
            [('startTest', subunit.RemotedTestCase("old mcdonald"))])

    def test_indented_test_colon_ignored(self):
        ignored_line = _b(" test: old mcdonald\n")
        self.protocol.lineReceived(ignored_line)
        self.assertEqual([], self.client._events)
        self.assertEqual(self.stream.getvalue(), ignored_line)

    def test_start_testing_colon(self):
        self.protocol.lineReceived(_b("testing: old mcdonald\n"))
        self.assertEqual(self.client._events,
            [('startTest', subunit.RemotedTestCase("old mcdonald"))])


class TestTestProtocolServerPassThrough(unittest.TestCase):

    def setUp(self):
        self.stdout = BytesIO()
        self.test = subunit.RemotedTestCase("old mcdonald")
        self.client = ExtendedTestResult()
        self.protocol = subunit.TestProtocolServer(self.client, self.stdout)

    def keywords_before_test(self):
        self.protocol.lineReceived(_b("failure a\n"))
        self.protocol.lineReceived(_b("failure: a\n"))
        self.protocol.lineReceived(_b("error a\n"))
        self.protocol.lineReceived(_b("error: a\n"))
        self.protocol.lineReceived(_b("success a\n"))
        self.protocol.lineReceived(_b("success: a\n"))
        self.protocol.lineReceived(_b("successful a\n"))
        self.protocol.lineReceived(_b("successful: a\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.assertEqual(self.stdout.getvalue(), _b("failure a\n"
                                                 "failure: a\n"
                                                 "error a\n"
                                                 "error: a\n"
                                                 "success a\n"
                                                 "success: a\n"
                                                 "successful a\n"
                                                 "successful: a\n"
                                                 "]\n"))

    def test_keywords_before_test(self):
        self.keywords_before_test()
        self.assertEqual(self.client._events, [])

    def test_keywords_after_error(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("error old mcdonald\n"))
        self.keywords_before_test()
        self.assertEqual([
            ('startTest', self.test),
            ('addError', self.test, {}),
            ('stopTest', self.test),
            ], self.client._events)

    def test_keywords_after_failure(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("failure old mcdonald\n"))
        self.keywords_before_test()
        self.assertEqual(self.client._events, [
            ('startTest', self.test),
            ('addFailure', self.test, {}),
            ('stopTest', self.test),
            ])

    def test_keywords_after_success(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("success old mcdonald\n"))
        self.keywords_before_test()
        self.assertEqual([
            ('startTest', self.test),
            ('addSuccess', self.test),
            ('stopTest', self.test),
            ], self.client._events)

    def test_keywords_after_test(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("failure a\n"))
        self.protocol.lineReceived(_b("failure: a\n"))
        self.protocol.lineReceived(_b("error a\n"))
        self.protocol.lineReceived(_b("error: a\n"))
        self.protocol.lineReceived(_b("success a\n"))
        self.protocol.lineReceived(_b("success: a\n"))
        self.protocol.lineReceived(_b("successful a\n"))
        self.protocol.lineReceived(_b("successful: a\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.protocol.lineReceived(_b("failure old mcdonald\n"))
        self.assertEqual(self.stdout.getvalue(), _b("test old mcdonald\n"
                                                 "failure a\n"
                                                 "failure: a\n"
                                                 "error a\n"
                                                 "error: a\n"
                                                 "success a\n"
                                                 "success: a\n"
                                                 "successful a\n"
                                                 "successful: a\n"
                                                 "]\n"))
        self.assertEqual(self.client._events, [
            ('startTest', self.test),
            ('addFailure', self.test, {}),
            ('stopTest', self.test),
            ])

    def test_keywords_during_failure(self):
        # A smoke test to make sure that the details parsers have control
        # appropriately.
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("failure: old mcdonald [\n"))
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("failure a\n"))
        self.protocol.lineReceived(_b("failure: a\n"))
        self.protocol.lineReceived(_b("error a\n"))
        self.protocol.lineReceived(_b("error: a\n"))
        self.protocol.lineReceived(_b("success a\n"))
        self.protocol.lineReceived(_b("success: a\n"))
        self.protocol.lineReceived(_b("successful a\n"))
        self.protocol.lineReceived(_b("successful: a\n"))
        self.protocol.lineReceived(_b(" ]\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.assertEqual(self.stdout.getvalue(), _b(""))
        details = {}
        details['traceback'] = Content(ContentType("text", "x-traceback",
            {'charset': 'utf8'}),
            lambda:[_b(
            "test old mcdonald\n"
            "failure a\n"
            "failure: a\n"
            "error a\n"
            "error: a\n"
            "success a\n"
            "success: a\n"
            "successful a\n"
            "successful: a\n"
            "]\n")])
        self.assertEqual(self.client._events, [
            ('startTest', self.test),
            ('addFailure', self.test, details),
            ('stopTest', self.test),
            ])

    def test_stdout_passthrough(self):
        """Lines received which cannot be interpreted as any protocol action
        should be passed through to sys.stdout.
        """
        bytes = _b("randombytes\n")
        self.protocol.lineReceived(bytes)
        self.assertEqual(self.stdout.getvalue(), bytes)


class TestTestProtocolServerLostConnection(unittest.TestCase):

    def setUp(self):
        self.client = Python26TestResult()
        self.protocol = subunit.TestProtocolServer(self.client)
        self.test = subunit.RemotedTestCase("old mcdonald")

    def test_lost_connection_no_input(self):
        self.protocol.lostConnection()
        self.assertEqual([], self.client._events)

    def test_lost_connection_after_start(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lostConnection()
        failure = subunit.RemoteError(
            _u("lost connection during test 'old mcdonald'"))
        self.assertEqual([
            ('startTest', self.test),
            ('addError', self.test, failure),
            ('stopTest', self.test),
            ], self.client._events)

    def test_lost_connected_after_error(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("error old mcdonald\n"))
        self.protocol.lostConnection()
        self.assertEqual([
            ('startTest', self.test),
            ('addError', self.test, subunit.RemoteError(_u(""))),
            ('stopTest', self.test),
            ], self.client._events)

    def do_connection_lost(self, outcome, opening):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("%s old mcdonald %s" % (outcome, opening)))
        self.protocol.lostConnection()
        failure = subunit.RemoteError(
            _u("lost connection during %s report of test 'old mcdonald'") %
            outcome)
        self.assertEqual([
            ('startTest', self.test),
            ('addError', self.test, failure),
            ('stopTest', self.test),
            ], self.client._events)

    def test_lost_connection_during_error(self):
        self.do_connection_lost("error", "[\n")

    def test_lost_connection_during_error_details(self):
        self.do_connection_lost("error", "[ multipart\n")

    def test_lost_connected_after_failure(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("failure old mcdonald\n"))
        self.protocol.lostConnection()
        self.assertEqual([
            ('startTest', self.test),
            ('addFailure', self.test, subunit.RemoteError(_u(""))),
            ('stopTest', self.test),
            ], self.client._events)

    def test_lost_connection_during_failure(self):
        self.do_connection_lost("failure", "[\n")

    def test_lost_connection_during_failure_details(self):
        self.do_connection_lost("failure", "[ multipart\n")

    def test_lost_connection_after_success(self):
        self.protocol.lineReceived(_b("test old mcdonald\n"))
        self.protocol.lineReceived(_b("success old mcdonald\n"))
        self.protocol.lostConnection()
        self.assertEqual([
            ('startTest', self.test),
            ('addSuccess', self.test),
            ('stopTest', self.test),
            ], self.client._events)

    def test_lost_connection_during_success(self):
        self.do_connection_lost("success", "[\n")

    def test_lost_connection_during_success_details(self):
        self.do_connection_lost("success", "[ multipart\n")

    def test_lost_connection_during_skip(self):
        self.do_connection_lost("skip", "[\n")

    def test_lost_connection_during_skip_details(self):
        self.do_connection_lost("skip", "[ multipart\n")

    def test_lost_connection_during_xfail(self):
        self.do_connection_lost("xfail", "[\n")

    def test_lost_connection_during_xfail_details(self):
        self.do_connection_lost("xfail", "[ multipart\n")

    def test_lost_connection_during_uxsuccess(self):
        self.do_connection_lost("uxsuccess", "[\n")

    def test_lost_connection_during_uxsuccess_details(self):
        self.do_connection_lost("uxsuccess", "[ multipart\n")


class TestInTestMultipart(unittest.TestCase):

    def setUp(self):
        self.client = ExtendedTestResult()
        self.protocol = subunit.TestProtocolServer(self.client)
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        self.test = subunit.RemotedTestCase(_u("mcdonalds farm"))

    def test__outcome_sets_details_parser(self):
        self.protocol._reading_success_details.details_parser = None
        self.protocol._state._outcome(0, _b("mcdonalds farm [ multipart\n"),
            None, self.protocol._reading_success_details)
        parser = self.protocol._reading_success_details.details_parser
        self.assertNotEqual(None, parser)
        self.assertTrue(isinstance(parser,
            subunit.details.MultipartDetailsParser))


class TestTestProtocolServerAddError(unittest.TestCase):

    def setUp(self):
        self.client = ExtendedTestResult()
        self.protocol = subunit.TestProtocolServer(self.client)
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        self.test = subunit.RemotedTestCase("mcdonalds farm")

    def simple_error_keyword(self, keyword):
        self.protocol.lineReceived(_b("%s mcdonalds farm\n" % keyword))
        details = {}
        self.assertEqual([
            ('startTest', self.test),
            ('addError', self.test, details),
            ('stopTest', self.test),
            ], self.client._events)

    def test_simple_error(self):
        self.simple_error_keyword("error")

    def test_simple_error_colon(self):
        self.simple_error_keyword("error:")

    def test_error_empty_message(self):
        self.protocol.lineReceived(_b("error mcdonalds farm [\n"))
        self.protocol.lineReceived(_b("]\n"))
        details = {}
        details['traceback'] = Content(ContentType("text", "x-traceback",
            {'charset': 'utf8'}), lambda:[_b("")])
        self.assertEqual([
            ('startTest', self.test),
            ('addError', self.test, details),
            ('stopTest', self.test),
            ], self.client._events)

    def error_quoted_bracket(self, keyword):
        self.protocol.lineReceived(_b("%s mcdonalds farm [\n" % keyword))
        self.protocol.lineReceived(_b(" ]\n"))
        self.protocol.lineReceived(_b("]\n"))
        details = {}
        details['traceback'] = Content(ContentType("text", "x-traceback",
            {'charset': 'utf8'}), lambda:[_b("]\n")])
        self.assertEqual([
            ('startTest', self.test),
            ('addError', self.test, details),
            ('stopTest', self.test),
            ], self.client._events)

    def test_error_quoted_bracket(self):
        self.error_quoted_bracket("error")

    def test_error_colon_quoted_bracket(self):
        self.error_quoted_bracket("error:")


class TestTestProtocolServerAddFailure(unittest.TestCase):

    def setUp(self):
        self.client = ExtendedTestResult()
        self.protocol = subunit.TestProtocolServer(self.client)
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        self.test = subunit.RemotedTestCase("mcdonalds farm")

    def assertFailure(self, details):
        self.assertEqual([
            ('startTest', self.test),
            ('addFailure', self.test, details),
            ('stopTest', self.test),
            ], self.client._events)

    def simple_failure_keyword(self, keyword):
        self.protocol.lineReceived(_b("%s mcdonalds farm\n" % keyword))
        details = {}
        self.assertFailure(details)

    def test_simple_failure(self):
        self.simple_failure_keyword("failure")

    def test_simple_failure_colon(self):
        self.simple_failure_keyword("failure:")

    def test_failure_empty_message(self):
        self.protocol.lineReceived(_b("failure mcdonalds farm [\n"))
        self.protocol.lineReceived(_b("]\n"))
        details = {}
        details['traceback'] = Content(ContentType("text", "x-traceback",
            {'charset': 'utf8'}), lambda:[_b("")])
        self.assertFailure(details)

    def failure_quoted_bracket(self, keyword):
        self.protocol.lineReceived(_b("%s mcdonalds farm [\n" % keyword))
        self.protocol.lineReceived(_b(" ]\n"))
        self.protocol.lineReceived(_b("]\n"))
        details = {}
        details['traceback'] = Content(ContentType("text", "x-traceback",
            {'charset': 'utf8'}), lambda:[_b("]\n")])
        self.assertFailure(details)

    def test_failure_quoted_bracket(self):
        self.failure_quoted_bracket("failure")

    def test_failure_colon_quoted_bracket(self):
        self.failure_quoted_bracket("failure:")


class TestTestProtocolServerAddxFail(unittest.TestCase):
    """Tests for the xfail keyword.

    In Python this can thunk through to Success due to stdlib limitations (see
    README).
    """

    def capture_expected_failure(self, test, err):
        self._events.append((test, err))

    def setup_python26(self):
        """Setup a test object ready to be xfailed and thunk to success."""
        self.client = Python26TestResult()
        self.setup_protocol()

    def setup_python27(self):
        """Setup a test object ready to be xfailed."""
        self.client = Python27TestResult()
        self.setup_protocol()

    def setup_python_ex(self):
        """Setup a test object ready to be xfailed with details."""
        self.client = ExtendedTestResult()
        self.setup_protocol()

    def setup_protocol(self):
        """Setup the protocol based on self.client."""
        self.protocol = subunit.TestProtocolServer(self.client)
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        self.test = self.client._events[-1][-1]

    def simple_xfail_keyword(self, keyword, as_success):
        self.protocol.lineReceived(_b("%s mcdonalds farm\n" % keyword))
        self.check_success_or_xfail(as_success)

    def check_success_or_xfail(self, as_success, error_message=None):
        if as_success:
            self.assertEqual([
                ('startTest', self.test),
                ('addSuccess', self.test),
                ('stopTest', self.test),
                ], self.client._events)
        else:
            details = {}
            if error_message is not None:
                details['traceback'] = Content(
                    ContentType("text", "x-traceback", {'charset': 'utf8'}),
                    lambda:[_b(error_message)])
            if isinstance(self.client, ExtendedTestResult):
                value = details
            else:
                if error_message is not None:
                    value = subunit.RemoteError(details_to_str(details))
                else:
                    value = subunit.RemoteError()
            self.assertEqual([
                ('startTest', self.test),
                ('addExpectedFailure', self.test, value),
                ('stopTest', self.test),
                ], self.client._events)

    def test_simple_xfail(self):
        self.setup_python26()
        self.simple_xfail_keyword("xfail", True)
        self.setup_python27()
        self.simple_xfail_keyword("xfail",  False)
        self.setup_python_ex()
        self.simple_xfail_keyword("xfail",  False)

    def test_simple_xfail_colon(self):
        self.setup_python26()
        self.simple_xfail_keyword("xfail:", True)
        self.setup_python27()
        self.simple_xfail_keyword("xfail:", False)
        self.setup_python_ex()
        self.simple_xfail_keyword("xfail:", False)

    def test_xfail_empty_message(self):
        self.setup_python26()
        self.empty_message(True)
        self.setup_python27()
        self.empty_message(False)
        self.setup_python_ex()
        self.empty_message(False, error_message="")

    def empty_message(self, as_success, error_message="\n"):
        self.protocol.lineReceived(_b("xfail mcdonalds farm [\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.check_success_or_xfail(as_success, error_message)

    def xfail_quoted_bracket(self, keyword, as_success):
        # This tests it is accepted, but cannot test it is used today, because
        # of not having a way to expose it in Python so far.
        self.protocol.lineReceived(_b("%s mcdonalds farm [\n" % keyword))
        self.protocol.lineReceived(_b(" ]\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.check_success_or_xfail(as_success, "]\n")

    def test_xfail_quoted_bracket(self):
        self.setup_python26()
        self.xfail_quoted_bracket("xfail", True)
        self.setup_python27()
        self.xfail_quoted_bracket("xfail", False)
        self.setup_python_ex()
        self.xfail_quoted_bracket("xfail", False)

    def test_xfail_colon_quoted_bracket(self):
        self.setup_python26()
        self.xfail_quoted_bracket("xfail:", True)
        self.setup_python27()
        self.xfail_quoted_bracket("xfail:", False)
        self.setup_python_ex()
        self.xfail_quoted_bracket("xfail:", False)


class TestTestProtocolServerAddunexpectedSuccess(TestCase):
    """Tests for the uxsuccess keyword."""

    def capture_expected_failure(self, test, err):
        self._events.append((test, err))

    def setup_python26(self):
        """Setup a test object ready to be xfailed and thunk to success."""
        self.client = Python26TestResult()
        self.setup_protocol()

    def setup_python27(self):
        """Setup a test object ready to be xfailed."""
        self.client = Python27TestResult()
        self.setup_protocol()

    def setup_python_ex(self):
        """Setup a test object ready to be xfailed with details."""
        self.client = ExtendedTestResult()
        self.setup_protocol()

    def setup_protocol(self):
        """Setup the protocol based on self.client."""
        self.protocol = subunit.TestProtocolServer(self.client)
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        self.test = self.client._events[-1][-1]

    def simple_uxsuccess_keyword(self, keyword, as_fail):
        self.protocol.lineReceived(_b("%s mcdonalds farm\n" % keyword))
        self.check_fail_or_uxsuccess(as_fail)

    def check_fail_or_uxsuccess(self, as_fail, error_message=None):
        details = {}
        if error_message is not None:
            details['traceback'] = Content(
                ContentType("text", "x-traceback", {'charset': 'utf8'}),
                lambda:[_b(error_message)])
        if isinstance(self.client, ExtendedTestResult):
            value = details
        else:
            value = None
        if as_fail:
            self.client._events[1] = self.client._events[1][:2]
            # The value is generated within the extended to original decorator:
            # todo use the testtools matcher to check on this.
            self.assertEqual([
                ('startTest', self.test),
                ('addFailure', self.test),
                ('stopTest', self.test),
                ], self.client._events)
        elif value:
            self.assertEqual([
                ('startTest', self.test),
                ('addUnexpectedSuccess', self.test, value),
                ('stopTest', self.test),
                ], self.client._events)
        else:
            self.assertEqual([
                ('startTest', self.test),
                ('addUnexpectedSuccess', self.test),
                ('stopTest', self.test),
                ], self.client._events)

    def test_simple_uxsuccess(self):
        self.setup_python26()
        self.simple_uxsuccess_keyword("uxsuccess", True)
        self.setup_python27()
        self.simple_uxsuccess_keyword("uxsuccess",  False)
        self.setup_python_ex()
        self.simple_uxsuccess_keyword("uxsuccess",  False)

    def test_simple_uxsuccess_colon(self):
        self.setup_python26()
        self.simple_uxsuccess_keyword("uxsuccess:", True)
        self.setup_python27()
        self.simple_uxsuccess_keyword("uxsuccess:", False)
        self.setup_python_ex()
        self.simple_uxsuccess_keyword("uxsuccess:", False)

    def test_uxsuccess_empty_message(self):
        self.setup_python26()
        self.empty_message(True)
        self.setup_python27()
        self.empty_message(False)
        self.setup_python_ex()
        self.empty_message(False, error_message="")

    def empty_message(self, as_fail, error_message="\n"):
        self.protocol.lineReceived(_b("uxsuccess mcdonalds farm [\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.check_fail_or_uxsuccess(as_fail, error_message)

    def uxsuccess_quoted_bracket(self, keyword, as_fail):
        self.protocol.lineReceived(_b("%s mcdonalds farm [\n" % keyword))
        self.protocol.lineReceived(_b(" ]\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.check_fail_or_uxsuccess(as_fail, "]\n")

    def test_uxsuccess_quoted_bracket(self):
        self.setup_python26()
        self.uxsuccess_quoted_bracket("uxsuccess", True)
        self.setup_python27()
        self.uxsuccess_quoted_bracket("uxsuccess", False)
        self.setup_python_ex()
        self.uxsuccess_quoted_bracket("uxsuccess", False)

    def test_uxsuccess_colon_quoted_bracket(self):
        self.setup_python26()
        self.uxsuccess_quoted_bracket("uxsuccess:", True)
        self.setup_python27()
        self.uxsuccess_quoted_bracket("uxsuccess:", False)
        self.setup_python_ex()
        self.uxsuccess_quoted_bracket("uxsuccess:", False)


class TestTestProtocolServerAddSkip(unittest.TestCase):
    """Tests for the skip keyword.

    In Python this meets the testtools extended TestResult contract.
    (See https://launchpad.net/testtools).
    """

    def setUp(self):
        """Setup a test object ready to be skipped."""
        self.client = ExtendedTestResult()
        self.protocol = subunit.TestProtocolServer(self.client)
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        self.test = self.client._events[-1][-1]

    def assertSkip(self, reason):
        details = {}
        if reason is not None:
            details['reason'] = Content(
                ContentType("text", "plain"), lambda:[reason])
        self.assertEqual([
            ('startTest', self.test),
            ('addSkip', self.test, details),
            ('stopTest', self.test),
            ], self.client._events)

    def simple_skip_keyword(self, keyword):
        self.protocol.lineReceived(_b("%s mcdonalds farm\n" % keyword))
        self.assertSkip(None)

    def test_simple_skip(self):
        self.simple_skip_keyword("skip")

    def test_simple_skip_colon(self):
        self.simple_skip_keyword("skip:")

    def test_skip_empty_message(self):
        self.protocol.lineReceived(_b("skip mcdonalds farm [\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.assertSkip(_b(""))

    def skip_quoted_bracket(self, keyword):
        # This tests it is accepted, but cannot test it is used today, because
        # of not having a way to expose it in Python so far.
        self.protocol.lineReceived(_b("%s mcdonalds farm [\n" % keyword))
        self.protocol.lineReceived(_b(" ]\n"))
        self.protocol.lineReceived(_b("]\n"))
        self.assertSkip(_b("]\n"))

    def test_skip_quoted_bracket(self):
        self.skip_quoted_bracket("skip")

    def test_skip_colon_quoted_bracket(self):
        self.skip_quoted_bracket("skip:")


class TestTestProtocolServerAddSuccess(unittest.TestCase):

    def setUp(self):
        self.client = ExtendedTestResult()
        self.protocol = subunit.TestProtocolServer(self.client)
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        self.test = subunit.RemotedTestCase("mcdonalds farm")

    def simple_success_keyword(self, keyword):
        self.protocol.lineReceived(_b("%s mcdonalds farm\n" % keyword))
        self.assertEqual([
            ('startTest', self.test),
            ('addSuccess', self.test),
            ('stopTest', self.test),
            ], self.client._events)

    def test_simple_success(self):
        self.simple_success_keyword("successful")

    def test_simple_success_colon(self):
        self.simple_success_keyword("successful:")

    def assertSuccess(self, details):
        self.assertEqual([
            ('startTest', self.test),
            ('addSuccess', self.test, details),
            ('stopTest', self.test),
            ], self.client._events)

    def test_success_empty_message(self):
        self.protocol.lineReceived(_b("success mcdonalds farm [\n"))
        self.protocol.lineReceived(_b("]\n"))
        details = {}
        details['message'] = Content(ContentType("text", "plain"),
            lambda:[_b("")])
        self.assertSuccess(details)

    def success_quoted_bracket(self, keyword):
        # This tests it is accepted, but cannot test it is used today, because
        # of not having a way to expose it in Python so far.
        self.protocol.lineReceived(_b("%s mcdonalds farm [\n" % keyword))
        self.protocol.lineReceived(_b(" ]\n"))
        self.protocol.lineReceived(_b("]\n"))
        details = {}
        details['message'] = Content(ContentType("text", "plain"),
            lambda:[_b("]\n")])
        self.assertSuccess(details)

    def test_success_quoted_bracket(self):
        self.success_quoted_bracket("success")

    def test_success_colon_quoted_bracket(self):
        self.success_quoted_bracket("success:")


class TestTestProtocolServerProgress(unittest.TestCase):
    """Test receipt of progress: directives."""

    def test_progress_accepted_stdlib(self):
        self.result = Python26TestResult()
        self.stream = BytesIO()
        self.protocol = subunit.TestProtocolServer(self.result,
            stream=self.stream)
        self.protocol.lineReceived(_b("progress: 23"))
        self.protocol.lineReceived(_b("progress: -2"))
        self.protocol.lineReceived(_b("progress: +4"))
        self.assertEqual(_b(""), self.stream.getvalue())

    def test_progress_accepted_extended(self):
        # With a progress capable TestResult, progress events are emitted.
        self.result = ExtendedTestResult()
        self.stream = BytesIO()
        self.protocol = subunit.TestProtocolServer(self.result,
            stream=self.stream)
        self.protocol.lineReceived(_b("progress: 23"))
        self.protocol.lineReceived(_b("progress: push"))
        self.protocol.lineReceived(_b("progress: -2"))
        self.protocol.lineReceived(_b("progress: pop"))
        self.protocol.lineReceived(_b("progress: +4"))
        self.assertEqual(_b(""), self.stream.getvalue())
        self.assertEqual([
            ('progress', 23, subunit.PROGRESS_SET),
            ('progress', None, subunit.PROGRESS_PUSH),
            ('progress', -2, subunit.PROGRESS_CUR),
            ('progress', None, subunit.PROGRESS_POP),
            ('progress', 4, subunit.PROGRESS_CUR),
            ], self.result._events)


class TestTestProtocolServerStreamTags(unittest.TestCase):
    """Test managing tags on the protocol level."""

    def setUp(self):
        self.client = ExtendedTestResult()
        self.protocol = subunit.TestProtocolServer(self.client)

    def test_initial_tags(self):
        self.protocol.lineReceived(_b("tags: foo bar:baz  quux\n"))
        self.assertEqual([
            ('tags', set(["foo", "bar:baz", "quux"]), set()),
            ], self.client._events)

    def test_minus_removes_tags(self):
        self.protocol.lineReceived(_b("tags: -bar quux\n"))
        self.assertEqual([
            ('tags', set(["quux"]), set(["bar"])),
            ], self.client._events)

    def test_tags_do_not_get_set_on_test(self):
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        test = self.client._events[0][-1]
        self.assertEqual(None, getattr(test, 'tags', None))

    def test_tags_do_not_get_set_on_global_tags(self):
        self.protocol.lineReceived(_b("tags: foo bar\n"))
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        test = self.client._events[-1][-1]
        self.assertEqual(None, getattr(test, 'tags', None))

    def test_tags_get_set_on_test_tags(self):
        self.protocol.lineReceived(_b("test mcdonalds farm\n"))
        test = self.client._events[-1][-1]
        self.protocol.lineReceived(_b("tags: foo bar\n"))
        self.protocol.lineReceived(_b("success mcdonalds farm\n"))
        self.assertEqual(None, getattr(test, 'tags', None))


class TestTestProtocolServerStreamTime(unittest.TestCase):
    """Test managing time information at the protocol level."""

    def test_time_accepted_stdlib(self):
        self.result = Python26TestResult()
        self.stream = BytesIO()
        self.protocol = subunit.TestProtocolServer(self.result,
            stream=self.stream)
        self.protocol.lineReceived(_b("time: 2001-12-12 12:59:59Z\n"))
        self.assertEqual(_b(""), self.stream.getvalue())

    def test_time_accepted_extended(self):
        self.result = ExtendedTestResult()
        self.stream = BytesIO()
        self.protocol = subunit.TestProtocolServer(self.result,
            stream=self.stream)
        self.protocol.lineReceived(_b("time: 2001-12-12 12:59:59Z\n"))
        self.assertEqual(_b(""), self.stream.getvalue())
        self.assertEqual([
            ('time', datetime.datetime(2001, 12, 12, 12, 59, 59, 0,
            iso8601.Utc()))
            ], self.result._events)


class TestRemotedTestCase(unittest.TestCase):

    def test_simple(self):
        test = subunit.RemotedTestCase("A test description")
        self.assertRaises(NotImplementedError, test.setUp)
        self.assertRaises(NotImplementedError, test.tearDown)
        self.assertEqual("A test description",
                         test.shortDescription())
        self.assertEqual("A test description",
                         test.id())
        self.assertEqual("A test description (subunit.RemotedTestCase)", "%s" % test)
        self.assertEqual("<subunit.RemotedTestCase description="
                         "'A test description'>", "%r" % test)
        result = unittest.TestResult()
        test.run(result)
        self.assertEqual([(test, _remote_exception_repr + ": "
                                 "Cannot run RemotedTestCases.\n\n")],
                         result.errors)
        self.assertEqual(1, result.testsRun)
        another_test = subunit.RemotedTestCase("A test description")
        self.assertEqual(test, another_test)
        different_test = subunit.RemotedTestCase("ofo")
        self.assertNotEqual(test, different_test)
        self.assertNotEqual(another_test, different_test)


class TestRemoteError(unittest.TestCase):

    def test_eq(self):
        error = subunit.RemoteError(_u("Something went wrong"))
        another_error = subunit.RemoteError(_u("Something went wrong"))
        different_error = subunit.RemoteError(_u("boo!"))
        self.assertEqual(error, another_error)
        self.assertNotEqual(error, different_error)
        self.assertNotEqual(different_error, another_error)

    def test_empty_constructor(self):
        self.assertEqual(subunit.RemoteError(), subunit.RemoteError(_u("")))


class TestExecTestCase(unittest.TestCase):

    class SampleExecTestCase(subunit.ExecTestCase):

        def test_sample_method(self):
            """sample-script.py"""
            # the sample script runs three tests, one each
            # that fails, errors and succeeds

        def test_sample_method_args(self):
            """sample-script.py foo"""
            # sample that will run just one test.

    def test_construct(self):
        test = self.SampleExecTestCase("test_sample_method")
        self.assertEqual(test.script,
                         subunit.join_dir(__file__, 'sample-script.py'))

    def test_args(self):
        result = unittest.TestResult()
        test = self.SampleExecTestCase("test_sample_method_args")
        test.run(result)
        self.assertEqual(1, result.testsRun)

    def test_run(self):
        result = ExtendedTestResult()
        test = self.SampleExecTestCase("test_sample_method")
        test.run(result)
        mcdonald = subunit.RemotedTestCase("old mcdonald")
        bing = subunit.RemotedTestCase("bing crosby")
        bing_details = {}
        bing_details['traceback'] = Content(ContentType("text", "x-traceback",
            {'charset': 'utf8'}), lambda:[_b("foo.c:53:ERROR invalid state\n")])
        an_error = subunit.RemotedTestCase("an error")
        error_details = {}
        self.assertEqual([
            ('startTest', mcdonald),
            ('addSuccess', mcdonald),
            ('stopTest', mcdonald),
            ('startTest', bing),
            ('addFailure', bing, bing_details),
            ('stopTest', bing),
            ('startTest', an_error),
            ('addError', an_error, error_details),
            ('stopTest', an_error),
            ], result._events)

    def test_debug(self):
        test = self.SampleExecTestCase("test_sample_method")
        test.debug()

    def test_count_test_cases(self):
        """TODO run the child process and count responses to determine the count."""

    def test_join_dir(self):
        sibling = subunit.join_dir(__file__, 'foo')
        filedir = os.path.abspath(os.path.dirname(__file__))
        expected = os.path.join(filedir, 'foo')
        self.assertEqual(sibling, expected)


class DoExecTestCase(subunit.ExecTestCase):

    def test_working_script(self):
        """sample-two-script.py"""


class TestIsolatedTestCase(TestCase):

    class SampleIsolatedTestCase(subunit.IsolatedTestCase):

        SETUP = False
        TEARDOWN = False
        TEST = False

        def setUp(self):
            TestIsolatedTestCase.SampleIsolatedTestCase.SETUP = True

        def tearDown(self):
            TestIsolatedTestCase.SampleIsolatedTestCase.TEARDOWN = True

        def test_sets_global_state(self):
            TestIsolatedTestCase.SampleIsolatedTestCase.TEST = True


    def test_construct(self):
        self.SampleIsolatedTestCase("test_sets_global_state")

    @skipIf(os.name != "posix", "Need a posix system for forking tests")
    def test_run(self):
        result = unittest.TestResult()
        test = self.SampleIsolatedTestCase("test_sets_global_state")
        test.run(result)
        self.assertEqual(result.testsRun, 1)
        self.assertEqual(self.SampleIsolatedTestCase.SETUP, False)
        self.assertEqual(self.SampleIsolatedTestCase.TEARDOWN, False)
        self.assertEqual(self.SampleIsolatedTestCase.TEST, False)

    def test_debug(self):
        pass
        #test = self.SampleExecTestCase("test_sample_method")
        #test.debug()


class TestIsolatedTestSuite(TestCase):

    class SampleTestToIsolate(unittest.TestCase):

        SETUP = False
        TEARDOWN = False
        TEST = False

        def setUp(self):
            TestIsolatedTestSuite.SampleTestToIsolate.SETUP = True

        def tearDown(self):
            TestIsolatedTestSuite.SampleTestToIsolate.TEARDOWN = True

        def test_sets_global_state(self):
            TestIsolatedTestSuite.SampleTestToIsolate.TEST = True


    def test_construct(self):
        subunit.IsolatedTestSuite()

    @skipIf(os.name != "posix", "Need a posix system for forking tests")
    def test_run(self):
        result = unittest.TestResult()
        suite = subunit.IsolatedTestSuite()
        sub_suite = unittest.TestSuite()
        sub_suite.addTest(self.SampleTestToIsolate("test_sets_global_state"))
        sub_suite.addTest(self.SampleTestToIsolate("test_sets_global_state"))
        suite.addTest(sub_suite)
        suite.addTest(self.SampleTestToIsolate("test_sets_global_state"))
        suite.run(result)
        self.assertEqual(result.testsRun, 3)
        self.assertEqual(self.SampleTestToIsolate.SETUP, False)
        self.assertEqual(self.SampleTestToIsolate.TEARDOWN, False)
        self.assertEqual(self.SampleTestToIsolate.TEST, False)


class TestTestProtocolClient(TestCase):

    def setUp(self):
        super(TestTestProtocolClient, self).setUp()
        self.io = BytesIO()
        self.protocol = subunit.TestProtocolClient(self.io)
        self.unicode_test = PlaceHolder(_u('\u2603'))
        self.test = TestTestProtocolClient("test_start_test")
        self.sample_details = {'something':Content(
            ContentType('text', 'plain'), lambda:[_b('serialised\nform')])}
        self.sample_tb_details = dict(self.sample_details)
        self.sample_tb_details['traceback'] = TracebackContent(
            subunit.RemoteError(_u("boo qux")), self.test)

    def test_start_test(self):
        """Test startTest on a TestProtocolClient."""
        self.protocol.startTest(self.test)
        self.assertEqual(self.io.getvalue(), _b("test: %s\n" % self.test.id()))

    def test_start_test_unicode_id(self):
        """Test startTest on a TestProtocolClient."""
        self.protocol.startTest(self.unicode_test)
        expected = _b("test: ") + _u('\u2603').encode('utf8') + _b("\n")
        self.assertEqual(expected, self.io.getvalue())

    def test_stop_test(self):
        # stopTest doesn't output anything.
        self.protocol.stopTest(self.test)
        self.assertEqual(self.io.getvalue(), _b(""))

    def test_add_success(self):
        """Test addSuccess on a TestProtocolClient."""
        self.protocol.addSuccess(self.test)
        self.assertEqual(
            self.io.getvalue(), _b("successful: %s\n" % self.test.id()))

    def test_add_outcome_unicode_id(self):
        """Test addSuccess on a TestProtocolClient."""
        self.protocol.addSuccess(self.unicode_test)
        expected = _b("successful: ") + _u('\u2603').encode('utf8') + _b("\n")
        self.assertEqual(expected, self.io.getvalue())

    def test_add_success_details(self):
        """Test addSuccess on a TestProtocolClient with details."""
        self.protocol.addSuccess(self.test, details=self.sample_details)
        self.assertEqual(
            self.io.getvalue(), _b("successful: %s [ multipart\n"
                "Content-Type: text/plain\n"
                "something\n"
                "F\r\nserialised\nform0\r\n]\n" % self.test.id()))

    def test_add_failure(self):
        """Test addFailure on a TestProtocolClient."""
        self.protocol.addFailure(
            self.test, subunit.RemoteError(_u("boo qux")))
        self.assertEqual(
            self.io.getvalue(),
            _b(('failure: %s [\n' + _remote_exception_str + ': boo qux\n]\n')
            % self.test.id()))

    def test_add_failure_details(self):
        """Test addFailure on a TestProtocolClient with details."""
        self.protocol.addFailure(
            self.test, details=self.sample_tb_details)
        self.assertThat([
            _b(("failure: %s [ multipart\n"
            "Content-Type: text/plain\n"
            "something\n"
            "F\r\nserialised\nform0\r\n"
            "Content-Type: text/x-traceback;charset=utf8,language=python\n"
            "traceback\n" + _remote_exception_str_chunked +
            "]\n") % self.test.id()),
            _b(("failure: %s [ multipart\n"
            "Content-Type: text/plain\n"
            "something\n"
            "F\r\nserialised\nform0\r\n"
            "Content-Type: text/x-traceback;language=python,charset=utf8\n"
            "traceback\n" + _remote_exception_str_chunked +
            "]\n") % self.test.id()),
            ],
            Contains(self.io.getvalue())),

    def test_add_error(self):
        """Test stopTest on a TestProtocolClient."""
        self.protocol.addError(
            self.test, subunit.RemoteError(_u("phwoar crikey")))
        self.assertEqual(
            self.io.getvalue(),
            _b(('error: %s [\n' +
            _remote_exception_str + ": phwoar crikey\n"
            "]\n") % self.test.id()))

    def test_add_error_details(self):
        """Test stopTest on a TestProtocolClient with details."""
        self.protocol.addError(
            self.test, details=self.sample_tb_details)
        self.assertThat([
            _b(("error: %s [ multipart\n"
            "Content-Type: text/plain\n"
            "something\n"
            "F\r\nserialised\nform0\r\n"
            "Content-Type: text/x-traceback;charset=utf8,language=python\n"
            "traceback\n" + _remote_exception_str_chunked +
            "]\n") % self.test.id()),
            _b(("error: %s [ multipart\n"
            "Content-Type: text/plain\n"
            "something\n"
            "F\r\nserialised\nform0\r\n"
            "Content-Type: text/x-traceback;language=python,charset=utf8\n"
            "traceback\n" + _remote_exception_str_chunked +
            "]\n") % self.test.id()),
            ],
            Contains(self.io.getvalue())),

    def test_add_expected_failure(self):
        """Test addExpectedFailure on a TestProtocolClient."""
        self.protocol.addExpectedFailure(
            self.test, subunit.RemoteError(_u("phwoar crikey")))
        self.assertEqual(
            self.io.getvalue(),
            _b(('xfail: %s [\n' +
            _remote_exception_str + ": phwoar crikey\n"
            "]\n") % self.test.id()))

    def test_add_expected_failure_details(self):
        """Test addExpectedFailure on a TestProtocolClient with details."""
        self.protocol.addExpectedFailure(
            self.test, details=self.sample_tb_details)
        self.assertThat([
            _b(("xfail: %s [ multipart\n"
            "Content-Type: text/plain\n"
            "something\n"
            "F\r\nserialised\nform0\r\n"
            "Content-Type: text/x-traceback;charset=utf8,language=python\n"
            "traceback\n" + _remote_exception_str_chunked +
            "]\n") % self.test.id()),
            _b(("xfail: %s [ multipart\n"
            "Content-Type: text/plain\n"
            "something\n"
            "F\r\nserialised\nform0\r\n"
            "Content-Type: text/x-traceback;language=python,charset=utf8\n"
            "traceback\n" + _remote_exception_str_chunked +
            "]\n") % self.test.id()),
            ],
            Contains(self.io.getvalue())),

    def test_add_skip(self):
        """Test addSkip on a TestProtocolClient."""
        self.protocol.addSkip(
            self.test, "Has it really?")
        self.assertEqual(
            self.io.getvalue(),
            _b('skip: %s [\nHas it really?\n]\n' % self.test.id()))

    def test_add_skip_details(self):
        """Test addSkip on a TestProtocolClient with details."""
        details = {'reason':Content(
            ContentType('text', 'plain'), lambda:[_b('Has it really?')])}
        self.protocol.addSkip(self.test, details=details)
        self.assertEqual(
            self.io.getvalue(),
            _b("skip: %s [ multipart\n"
            "Content-Type: text/plain\n"
            "reason\n"
            "E\r\nHas it really?0\r\n"
            "]\n" % self.test.id()))

    def test_progress_set(self):
        self.protocol.progress(23, subunit.PROGRESS_SET)
        self.assertEqual(self.io.getvalue(), _b('progress: 23\n'))

    def test_progress_neg_cur(self):
        self.protocol.progress(-23, subunit.PROGRESS_CUR)
        self.assertEqual(self.io.getvalue(), _b('progress: -23\n'))

    def test_progress_pos_cur(self):
        self.protocol.progress(23, subunit.PROGRESS_CUR)
        self.assertEqual(self.io.getvalue(), _b('progress: +23\n'))

    def test_progress_pop(self):
        self.protocol.progress(1234, subunit.PROGRESS_POP)
        self.assertEqual(self.io.getvalue(), _b('progress: pop\n'))

    def test_progress_push(self):
        self.protocol.progress(1234, subunit.PROGRESS_PUSH)
        self.assertEqual(self.io.getvalue(), _b('progress: push\n'))

    def test_time(self):
        # Calling time() outputs a time signal immediately.
        self.protocol.time(
            datetime.datetime(2009,10,11,12,13,14,15, iso8601.Utc()))
        self.assertEqual(
            _b("time: 2009-10-11 12:13:14.000015Z\n"),
            self.io.getvalue())

    def test_add_unexpected_success(self):
        """Test addUnexpectedSuccess on a TestProtocolClient."""
        self.protocol.addUnexpectedSuccess(self.test)
        self.assertEqual(
            self.io.getvalue(), _b("uxsuccess: %s\n" % self.test.id()))

    def test_add_unexpected_success_details(self):
        """Test addUnexpectedSuccess on a TestProtocolClient with details."""
        self.protocol.addUnexpectedSuccess(self.test, details=self.sample_details)
        self.assertEqual(
            self.io.getvalue(), _b("uxsuccess: %s [ multipart\n"
                "Content-Type: text/plain\n"
                "something\n"
                "F\r\nserialised\nform0\r\n]\n" % self.test.id()))

    def test_tags_empty(self):
        self.protocol.tags(set(), set())
        self.assertEqual(_b(""), self.io.getvalue())

    def test_tags_add(self):
        self.protocol.tags(set(['foo']), set())
        self.assertEqual(_b("tags: foo\n"), self.io.getvalue())

    def test_tags_both(self):
        self.protocol.tags(set(['quux']), set(['bar']))
        self.assertThat(
            [b"tags: quux -bar\n", b"tags: -bar quux\n"],
            Contains(self.io.getvalue()))

    def test_tags_gone(self):
        self.protocol.tags(set(), set(['bar']))
        self.assertEqual(_b("tags: -bar\n"), self.io.getvalue())
