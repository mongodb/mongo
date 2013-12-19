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

"""Tests for subunit.tag_stream."""

from io import BytesIO

import testtools
from testtools.matchers import Contains

import subunit
import subunit.test_results


class TestSubUnitTags(testtools.TestCase):

    def setUp(self):
        super(TestSubUnitTags, self).setUp()
        self.original = BytesIO()
        self.filtered = BytesIO()

    def test_add_tag(self):
        # Literal values to avoid set sort-order dependencies. Python code show
        # derivation.
        # reference = BytesIO()
        # stream = subunit.StreamResultToBytes(reference)
        # stream.status(
        #     test_id='test', test_status='inprogress', test_tags=set(['quux', 'foo']))
        # stream.status(
        #     test_id='test', test_status='success', test_tags=set(['bar', 'quux', 'foo']))
        reference = [
            b'\xb3)\x82\x17\x04test\x02\x04quux\x03foo\x05\x97n\x86\xb3)'
                b'\x83\x1b\x04test\x03\x03bar\x04quux\x03fooqn\xab)',
            b'\xb3)\x82\x17\x04test\x02\x04quux\x03foo\x05\x97n\x86\xb3)'
                b'\x83\x1b\x04test\x03\x04quux\x03foo\x03bar\xaf\xbd\x9d\xd6',
            b'\xb3)\x82\x17\x04test\x02\x04quux\x03foo\x05\x97n\x86\xb3)'
                b'\x83\x1b\x04test\x03\x04quux\x03bar\x03foo\x03\x04b\r',
            b'\xb3)\x82\x17\x04test\x02\x04quux\x03foo\x05\x97n\x86\xb3)'
                b'\x83\x1b\x04test\x03\x03bar\x03foo\x04quux\xd2\x18\x1bC',
            b'\xb3)\x82\x17\x04test\x02\x03foo\x04quux\xa6\xe1\xde\xec\xb3)'
                b'\x83\x1b\x04test\x03\x03foo\x04quux\x03bar\x08\xc2X\x83',
            b'\xb3)\x82\x17\x04test\x02\x03foo\x04quux\xa6\xe1\xde\xec\xb3)'
                b'\x83\x1b\x04test\x03\x03bar\x03foo\x04quux\xd2\x18\x1bC',
            b'\xb3)\x82\x17\x04test\x02\x03foo\x04quux\xa6\xe1\xde\xec\xb3)'
                b'\x83\x1b\x04test\x03\x03foo\x03bar\x04quux:\x05e\x80',
            ]
        stream = subunit.StreamResultToBytes(self.original)
        stream.status(
            test_id='test', test_status='inprogress', test_tags=set(['foo']))
        stream.status(
            test_id='test', test_status='success', test_tags=set(['foo', 'bar']))
        self.original.seek(0)
        self.assertEqual(
            0, subunit.tag_stream(self.original, self.filtered, ["quux"]))
        self.assertThat(reference, Contains(self.filtered.getvalue()))

    def test_remove_tag(self):
        reference = BytesIO()
        stream = subunit.StreamResultToBytes(reference)
        stream.status(
            test_id='test', test_status='inprogress', test_tags=set(['foo']))
        stream.status(
            test_id='test', test_status='success', test_tags=set(['foo']))
        stream = subunit.StreamResultToBytes(self.original)
        stream.status(
            test_id='test', test_status='inprogress', test_tags=set(['foo']))
        stream.status(
            test_id='test', test_status='success', test_tags=set(['foo', 'bar']))
        self.original.seek(0)
        self.assertEqual(
            0, subunit.tag_stream(self.original, self.filtered, ["-bar"]))
        self.assertEqual(reference.getvalue(), self.filtered.getvalue())
