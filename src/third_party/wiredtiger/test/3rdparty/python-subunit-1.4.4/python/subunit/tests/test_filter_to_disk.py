#
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2013 Subunit Contributors
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

import io
import os.path

from fixtures import TempDir
from testtools import TestCase
from testtools.matchers import FileContains

from subunit import _to_disk
from subunit.v2 import StreamResultToBytes


class SmokeTest(TestCase):

    def test_smoke(self):
        output = os.path.join(self.useFixture(TempDir()).path, 'output')
        stdin = io.BytesIO()
        stdout = io.StringIO()
        writer = StreamResultToBytes(stdin)
        writer.startTestRun()
        writer.status(
            'foo', 'success', {'tag'}, file_name='fred',
            file_bytes=b'abcdefg', eof=True, mime_type='text/plain')
        writer.stopTestRun()
        stdin.seek(0)
        _to_disk.to_disk(['-d', output], stdin=stdin, stdout=stdout)
        self.expectThat(
            os.path.join(output, 'foo/test.json'),
            FileContains(
                '{"details": ["fred"], "id": "foo", "start": null, '
                '"status": "success", "stop": null, "tags": ["tag"]}'))
        self.expectThat(
            os.path.join(output, 'foo/fred'),
            FileContains('abcdefg'))
