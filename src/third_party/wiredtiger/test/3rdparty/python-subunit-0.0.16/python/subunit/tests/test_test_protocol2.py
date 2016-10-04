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

from io import BytesIO
import datetime

from testtools import TestCase
from testtools.matchers import Contains, HasLength
from testtools.tests.test_testresult import TestStreamResultContract
from testtools.testresult.doubles import StreamResult

import subunit
import subunit.iso8601 as iso8601

CONSTANT_ENUM = b'\xb3)\x01\x0c\x03foo\x08U_\x1b'
CONSTANT_INPROGRESS = b'\xb3)\x02\x0c\x03foo\x8e\xc1-\xb5'
CONSTANT_SUCCESS = b'\xb3)\x03\x0c\x03fooE\x9d\xfe\x10'
CONSTANT_UXSUCCESS = b'\xb3)\x04\x0c\x03fooX\x98\xce\xa8'
CONSTANT_SKIP = b'\xb3)\x05\x0c\x03foo\x93\xc4\x1d\r'
CONSTANT_FAIL = b'\xb3)\x06\x0c\x03foo\x15Po\xa3'
CONSTANT_XFAIL = b'\xb3)\x07\x0c\x03foo\xde\x0c\xbc\x06'
CONSTANT_EOF = b'\xb3!\x10\x08S\x15\x88\xdc'
CONSTANT_FILE_CONTENT = b'\xb3!@\x13\x06barney\x03wooA5\xe3\x8c'
CONSTANT_MIME = b'\xb3! #\x1aapplication/foo; charset=1x3Q\x15'
CONSTANT_TIMESTAMP = b'\xb3+\x03\x13<\x17T\xcf\x80\xaf\xc8\x03barI\x96>-'
CONSTANT_ROUTE_CODE = b'\xb3-\x03\x13\x03bar\x06source\x9cY9\x19'
CONSTANT_RUNNABLE = b'\xb3(\x03\x0c\x03foo\xe3\xea\xf5\xa4'
CONSTANT_TAGS = [
    b'\xb3)\x80\x15\x03bar\x02\x03foo\x03barTHn\xb4',
    b'\xb3)\x80\x15\x03bar\x02\x03bar\x03foo\xf8\xf1\x91o',
    ]


class TestStreamResultToBytesContract(TestCase, TestStreamResultContract):
    """Check that StreamResult behaves as testtools expects."""

    def _make_result(self):
        return subunit.StreamResultToBytes(BytesIO())


class TestStreamResultToBytes(TestCase):

    def _make_result(self):
        output = BytesIO()
        return subunit.StreamResultToBytes(output), output

    def test_numbers(self):
        result = subunit.StreamResultToBytes(BytesIO())
        packet = []
        self.assertRaises(Exception, result._write_number, -1, packet)
        self.assertEqual([], packet)
        result._write_number(0, packet)
        self.assertEqual([b'\x00'], packet)
        del packet[:]
        result._write_number(63, packet)
        self.assertEqual([b'\x3f'], packet)
        del packet[:]
        result._write_number(64, packet)
        self.assertEqual([b'\x40\x40'], packet)
        del packet[:]
        result._write_number(16383, packet)
        self.assertEqual([b'\x7f\xff'], packet)
        del packet[:]
        result._write_number(16384, packet)
        self.assertEqual([b'\x80\x40', b'\x00'], packet)
        del packet[:]
        result._write_number(4194303, packet)
        self.assertEqual([b'\xbf\xff', b'\xff'], packet)
        del packet[:]
        result._write_number(4194304, packet)
        self.assertEqual([b'\xc0\x40\x00\x00'], packet)
        del packet[:]
        result._write_number(1073741823, packet)
        self.assertEqual([b'\xff\xff\xff\xff'], packet)
        del packet[:]
        self.assertRaises(Exception, result._write_number, 1073741824, packet)
        self.assertEqual([], packet)

    def test_volatile_length(self):
        # if the length of the packet data before the length itself is
        # considered is right on the boundary for length's variable length
        # encoding, it is easy to get the length wrong by not accounting for
        # length itself.
        # that is, the encoder has to ensure that length == sum (length_of_rest
        # + length_of_length)
        result, output = self._make_result()
        # 1 byte short:
        result.status(file_name="", file_bytes=b'\xff'*0)
        self.assertThat(output.getvalue(), HasLength(10))
        self.assertEqual(b'\x0a', output.getvalue()[3:4])
        output.seek(0)
        output.truncate()
        # 1 byte long:
        result.status(file_name="", file_bytes=b'\xff'*53)
        self.assertThat(output.getvalue(), HasLength(63))
        self.assertEqual(b'\x3f', output.getvalue()[3:4])
        output.seek(0)
        output.truncate()
        # 2 bytes short
        result.status(file_name="", file_bytes=b'\xff'*54)
        self.assertThat(output.getvalue(), HasLength(65))
        self.assertEqual(b'\x40\x41', output.getvalue()[3:5])
        output.seek(0)
        output.truncate()
        # 2 bytes long
        result.status(file_name="", file_bytes=b'\xff'*16371)
        self.assertThat(output.getvalue(), HasLength(16383))
        self.assertEqual(b'\x7f\xff', output.getvalue()[3:5])
        output.seek(0)
        output.truncate()
        # 3 bytes short
        result.status(file_name="", file_bytes=b'\xff'*16372)
        self.assertThat(output.getvalue(), HasLength(16385))
        self.assertEqual(b'\x80\x40\x01', output.getvalue()[3:6])
        output.seek(0)
        output.truncate()
        # 3 bytes long
        result.status(file_name="", file_bytes=b'\xff'*4194289)
        self.assertThat(output.getvalue(), HasLength(4194303))
        self.assertEqual(b'\xbf\xff\xff', output.getvalue()[3:6])
        output.seek(0)
        output.truncate()
        self.assertRaises(Exception, result.status, file_name="",
            file_bytes=b'\xff'*4194290)

    def test_trivial_enumeration(self):
        result, output = self._make_result()
        result.status("foo", 'exists')
        self.assertEqual(CONSTANT_ENUM, output.getvalue())

    def test_inprogress(self):
        result, output = self._make_result()
        result.status("foo", 'inprogress')
        self.assertEqual(CONSTANT_INPROGRESS, output.getvalue())

    def test_success(self):
        result, output = self._make_result()
        result.status("foo", 'success')
        self.assertEqual(CONSTANT_SUCCESS, output.getvalue())

    def test_uxsuccess(self):
        result, output = self._make_result()
        result.status("foo", 'uxsuccess')
        self.assertEqual(CONSTANT_UXSUCCESS, output.getvalue())

    def test_skip(self):
        result, output = self._make_result()
        result.status("foo", 'skip')
        self.assertEqual(CONSTANT_SKIP, output.getvalue())

    def test_fail(self):
        result, output = self._make_result()
        result.status("foo", 'fail')
        self.assertEqual(CONSTANT_FAIL, output.getvalue())

    def test_xfail(self):
        result, output = self._make_result()
        result.status("foo", 'xfail')
        self.assertEqual(CONSTANT_XFAIL, output.getvalue())

    def test_unknown_status(self):
        result, output = self._make_result()
        self.assertRaises(Exception, result.status, "foo", 'boo')
        self.assertEqual(b'', output.getvalue())

    def test_eof(self):
        result, output = self._make_result()
        result.status(eof=True)
        self.assertEqual(CONSTANT_EOF, output.getvalue())

    def test_file_content(self):
        result, output = self._make_result()
        result.status(file_name="barney", file_bytes=b"woo")
        self.assertEqual(CONSTANT_FILE_CONTENT, output.getvalue())

    def test_mime(self):
        result, output = self._make_result()
        result.status(mime_type="application/foo; charset=1")
        self.assertEqual(CONSTANT_MIME, output.getvalue())

    def test_route_code(self):
        result, output = self._make_result()
        result.status(test_id="bar", test_status='success',
            route_code="source")
        self.assertEqual(CONSTANT_ROUTE_CODE, output.getvalue())

    def test_runnable(self):
        result, output = self._make_result()
        result.status("foo", 'success', runnable=False)
        self.assertEqual(CONSTANT_RUNNABLE, output.getvalue())

    def test_tags(self):
        result, output = self._make_result()
        result.status(test_id="bar", test_tags=set(['foo', 'bar']))
        self.assertThat(CONSTANT_TAGS, Contains(output.getvalue()))

    def test_timestamp(self):
        timestamp = datetime.datetime(2001, 12, 12, 12, 59, 59, 45,
            iso8601.Utc())
        result, output = self._make_result()
        result.status(test_id="bar", test_status='success', timestamp=timestamp)
        self.assertEqual(CONSTANT_TIMESTAMP, output.getvalue())


class TestByteStreamToStreamResult(TestCase):

    def test_non_subunit_encapsulated(self):
        source = BytesIO(b"foo\nbar\n")
        result = StreamResult()
        subunit.ByteStreamToStreamResult(
            source, non_subunit_name="stdout").run(result)
        self.assertEqual([
            ('status', None, None, None, True, 'stdout', b'f', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'o', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'o', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'\n', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'b', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'a', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'r', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'\n', False, None, None, None),
            ], result._events)
        self.assertEqual(b'', source.read())

    def test_signature_middle_utf8_char(self):
        utf8_bytes = b'\xe3\xb3\x8a'
        source = BytesIO(utf8_bytes)
        # Should be treated as one character (it is u'\u3cca') and wrapped
        result = StreamResult()
        subunit.ByteStreamToStreamResult(
            source, non_subunit_name="stdout").run(
            result)
        self.assertEqual([
            ('status', None, None, None, True, 'stdout', b'\xe3', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'\xb3', False, None, None, None),
            ('status', None, None, None, True, 'stdout', b'\x8a', False, None, None, None),
            ], result._events)

    def test_non_subunit_disabled_raises(self):
        source = BytesIO(b"foo\nbar\n")
        result = StreamResult()
        case = subunit.ByteStreamToStreamResult(source)
        e = self.assertRaises(Exception, case.run, result)
        self.assertEqual(b'f', e.args[1])
        self.assertEqual(b'oo\nbar\n', source.read())
        self.assertEqual([], result._events)

    def test_trivial_enumeration(self):
        source = BytesIO(CONSTANT_ENUM)
        result = StreamResult()
        subunit.ByteStreamToStreamResult(
            source, non_subunit_name="stdout").run(result)
        self.assertEqual(b'', source.read())
        self.assertEqual([
            ('status', 'foo', 'exists', None, True, None, None, False, None, None, None),
            ], result._events)

    def test_multiple_events(self):
        source = BytesIO(CONSTANT_ENUM + CONSTANT_ENUM)
        result = StreamResult()
        subunit.ByteStreamToStreamResult(
            source, non_subunit_name="stdout").run(result)
        self.assertEqual(b'', source.read())
        self.assertEqual([
            ('status', 'foo', 'exists', None, True, None, None, False, None, None, None),
            ('status', 'foo', 'exists', None, True, None, None, False, None, None, None),
            ], result._events)

    def test_inprogress(self):
        self.check_event(CONSTANT_INPROGRESS, 'inprogress')

    def test_success(self):
        self.check_event(CONSTANT_SUCCESS, 'success')

    def test_uxsuccess(self):
        self.check_event(CONSTANT_UXSUCCESS, 'uxsuccess')

    def test_skip(self):
        self.check_event(CONSTANT_SKIP, 'skip')

    def test_fail(self):
        self.check_event(CONSTANT_FAIL, 'fail')

    def test_xfail(self):
        self.check_event(CONSTANT_XFAIL, 'xfail')

    def check_events(self, source_bytes, events):
        source = BytesIO(source_bytes)
        result = StreamResult()
        subunit.ByteStreamToStreamResult(
            source, non_subunit_name="stdout").run(result)
        self.assertEqual(b'', source.read())
        self.assertEqual(events, result._events)
        #- any file attachments should be byte contents [as users assume that].
        for event in result._events:
            if event[5] is not None:
                self.assertIsInstance(event[6], bytes)

    def check_event(self, source_bytes, test_status=None, test_id="foo",
        route_code=None, timestamp=None, tags=None, mime_type=None,
        file_name=None, file_bytes=None, eof=False, runnable=True):
        event = self._event(test_id=test_id, test_status=test_status,
            tags=tags, runnable=runnable, file_name=file_name,
            file_bytes=file_bytes, eof=eof, mime_type=mime_type,
            route_code=route_code, timestamp=timestamp)
        self.check_events(source_bytes, [event])

    def _event(self, test_status=None, test_id=None, route_code=None,
        timestamp=None, tags=None, mime_type=None, file_name=None,
        file_bytes=None, eof=False, runnable=True):
        return ('status', test_id, test_status, tags, runnable, file_name,
            file_bytes, eof, mime_type, route_code, timestamp)

    def test_eof(self):
        self.check_event(CONSTANT_EOF, test_id=None, eof=True)

    def test_file_content(self):
        self.check_event(CONSTANT_FILE_CONTENT,
            test_id=None, file_name="barney", file_bytes=b"woo")

    def test_file_content_length_into_checksum(self):
        # A bad file content length which creeps into the checksum.
        bad_file_length_content = b'\xb3!@\x13\x06barney\x04woo\xdc\xe2\xdb\x35'
        self.check_events(bad_file_length_content, [
            self._event(test_id="subunit.parser", eof=True,
                file_name="Packet data", file_bytes=bad_file_length_content,
                mime_type="application/octet-stream"),
            self._event(test_id="subunit.parser", test_status="fail", eof=True,
                file_name="Parser Error",
                file_bytes=b"File content extends past end of packet: claimed 4 bytes, 3 available",
                mime_type="text/plain;charset=utf8"),
            ])

    def test_packet_length_4_word_varint(self):
        packet_data = b'\xb3!@\xc0\x00\x11'
        self.check_events(packet_data, [
            self._event(test_id="subunit.parser", eof=True,
                file_name="Packet data", file_bytes=packet_data,
                mime_type="application/octet-stream"),
            self._event(test_id="subunit.parser", test_status="fail", eof=True,
                file_name="Parser Error",
                file_bytes=b"3 byte maximum given but 4 byte value found.",
                mime_type="text/plain;charset=utf8"),
            ])

    def test_mime(self):
        self.check_event(CONSTANT_MIME,
            test_id=None, mime_type='application/foo; charset=1')

    def test_route_code(self):
        self.check_event(CONSTANT_ROUTE_CODE,
            'success', route_code="source", test_id="bar")

    def test_runnable(self):
        self.check_event(CONSTANT_RUNNABLE,
            test_status='success', runnable=False)

    def test_tags(self):
        self.check_event(CONSTANT_TAGS[0],
            None, tags=set(['foo', 'bar']), test_id="bar")

    def test_timestamp(self):
        timestamp = datetime.datetime(2001, 12, 12, 12, 59, 59, 45,
            iso8601.Utc())
        self.check_event(CONSTANT_TIMESTAMP,
            'success', test_id='bar', timestamp=timestamp)

    def test_bad_crc_errors_via_status(self):
        file_bytes = CONSTANT_MIME[:-1] + b'\x00'
        self.check_events( file_bytes, [
            self._event(test_id="subunit.parser", eof=True,
                file_name="Packet data", file_bytes=file_bytes,
                mime_type="application/octet-stream"),
            self._event(test_id="subunit.parser", test_status="fail", eof=True,
                file_name="Parser Error",
                file_bytes=b'Bad checksum - calculated (0x78335115), '
                    b'stored (0x78335100)',
                mime_type="text/plain;charset=utf8"),
            ])

    def test_not_utf8_in_string(self):
        file_bytes = CONSTANT_ROUTE_CODE[:5] + b'\xb4' + CONSTANT_ROUTE_CODE[6:-4] + b'\xce\x56\xc6\x17'
        self.check_events(file_bytes, [
            self._event(test_id="subunit.parser", eof=True,
                file_name="Packet data", file_bytes=file_bytes,
                mime_type="application/octet-stream"),
            self._event(test_id="subunit.parser", test_status="fail", eof=True,
                file_name="Parser Error",
                file_bytes=b'UTF8 string at offset 2 is not UTF8',
                mime_type="text/plain;charset=utf8"),
            ])

    def test_NULL_in_string(self):
        file_bytes = CONSTANT_ROUTE_CODE[:6] + b'\x00' + CONSTANT_ROUTE_CODE[7:-4] + b'\xd7\x41\xac\xfe'
        self.check_events(file_bytes, [
            self._event(test_id="subunit.parser", eof=True,
                file_name="Packet data", file_bytes=file_bytes,
                mime_type="application/octet-stream"),
            self._event(test_id="subunit.parser", test_status="fail", eof=True,
                file_name="Parser Error",
                file_bytes=b'UTF8 string at offset 2 contains NUL byte',
                mime_type="text/plain;charset=utf8"),
            ])

    def test_bad_utf8_stringlength(self):
        file_bytes = CONSTANT_ROUTE_CODE[:4] + b'\x3f' + CONSTANT_ROUTE_CODE[5:-4] + b'\xbe\x29\xe0\xc2'
        self.check_events(file_bytes, [
            self._event(test_id="subunit.parser", eof=True,
                file_name="Packet data", file_bytes=file_bytes,
                mime_type="application/octet-stream"),
            self._event(test_id="subunit.parser", test_status="fail", eof=True,
                file_name="Parser Error",
                file_bytes=b'UTF8 string at offset 2 extends past end of '
                    b'packet: claimed 63 bytes, 10 available',
                mime_type="text/plain;charset=utf8"),
            ])

    def test_route_code_and_file_content(self):
        content = BytesIO()
        subunit.StreamResultToBytes(content).status(
            route_code='0', mime_type='text/plain', file_name='bar',
            file_bytes=b'foo')
        self.check_event(content.getvalue(), test_id=None, file_name='bar',
            route_code='0', mime_type='text/plain', file_bytes=b'foo')
