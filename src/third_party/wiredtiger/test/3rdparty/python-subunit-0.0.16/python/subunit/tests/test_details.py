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

import unittest

from testtools.compat import _b, StringIO

import subunit.tests
from subunit import content, content_type, details


class TestSimpleDetails(unittest.TestCase):

    def test_lineReceived(self):
        parser = details.SimpleDetailsParser(None)
        parser.lineReceived(_b("foo\n"))
        parser.lineReceived(_b("bar\n"))
        self.assertEqual(_b("foo\nbar\n"), parser._message)

    def test_lineReceived_escaped_bracket(self):
        parser = details.SimpleDetailsParser(None)
        parser.lineReceived(_b("foo\n"))
        parser.lineReceived(_b(" ]are\n"))
        parser.lineReceived(_b("bar\n"))
        self.assertEqual(_b("foo\n]are\nbar\n"), parser._message)

    def test_get_message(self):
        parser = details.SimpleDetailsParser(None)
        self.assertEqual(_b(""), parser.get_message())

    def test_get_details(self):
        parser = details.SimpleDetailsParser(None)
        traceback = ""
        expected = {}
        expected['traceback'] = content.Content(
            content_type.ContentType("text", "x-traceback",
                {'charset': 'utf8'}),
            lambda:[_b("")])
        found = parser.get_details()
        self.assertEqual(expected.keys(), found.keys())
        self.assertEqual(expected['traceback'].content_type,
            found['traceback'].content_type)
        self.assertEqual(_b('').join(expected['traceback'].iter_bytes()),
            _b('').join(found['traceback'].iter_bytes()))

    def test_get_details_skip(self):
        parser = details.SimpleDetailsParser(None)
        traceback = ""
        expected = {}
        expected['reason'] = content.Content(
            content_type.ContentType("text", "plain"),
            lambda:[_b("")])
        found = parser.get_details("skip")
        self.assertEqual(expected, found)

    def test_get_details_success(self):
        parser = details.SimpleDetailsParser(None)
        traceback = ""
        expected = {}
        expected['message'] = content.Content(
            content_type.ContentType("text", "plain"),
            lambda:[_b("")])
        found = parser.get_details("success")
        self.assertEqual(expected, found)


class TestMultipartDetails(unittest.TestCase):

    def test_get_message_is_None(self):
        parser = details.MultipartDetailsParser(None)
        self.assertEqual(None, parser.get_message())

    def test_get_details(self):
        parser = details.MultipartDetailsParser(None)
        self.assertEqual({}, parser.get_details())

    def test_parts(self):
        parser = details.MultipartDetailsParser(None)
        parser.lineReceived(_b("Content-Type: text/plain\n"))
        parser.lineReceived(_b("something\n"))
        parser.lineReceived(_b("F\r\n"))
        parser.lineReceived(_b("serialised\n"))
        parser.lineReceived(_b("form0\r\n"))
        expected = {}
        expected['something'] = content.Content(
            content_type.ContentType("text", "plain"),
            lambda:[_b("serialised\nform")])
        found = parser.get_details()
        self.assertEqual(expected.keys(), found.keys())
        self.assertEqual(expected['something'].content_type,
            found['something'].content_type)
        self.assertEqual(_b('').join(expected['something'].iter_bytes()),
            _b('').join(found['something'].iter_bytes()))
