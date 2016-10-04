#
#  subunit: extensions to Python unittest to get test results from subprocesses.
#  Copyright (C) 2013  Robert Collins <robertc@robertcollins.net>
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

import sys
from tempfile import NamedTemporaryFile

from testtools import TestCase

from subunit.filters import find_stream


class TestFindStream(TestCase):

    def test_no_argv(self):
        self.assertEqual('foo', find_stream('foo', []))

    def test_opens_file(self):
        f = NamedTemporaryFile()
        f.write(b'foo')
        f.flush()
        stream = find_stream('bar', [f.name])
        self.assertEqual(b'foo', stream.read())
