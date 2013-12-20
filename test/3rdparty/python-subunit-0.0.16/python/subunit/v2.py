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

import codecs
utf_8_decode = codecs.utf_8_decode
import datetime
from io import UnsupportedOperation
import os
import select
import struct
import zlib

from extras import safe_hasattr, try_imports
builtins = try_imports(['__builtin__', 'builtins'])

import subunit
import subunit.iso8601 as iso8601

__all__ = [
    'ByteStreamToStreamResult',
    'StreamResultToBytes',
    ]

SIGNATURE = b'\xb3'
FMT_8  = '>B'
FMT_16 = '>H'
FMT_24 = '>HB'
FMT_32 = '>I'
FMT_TIMESTAMP = '>II'
FLAG_TEST_ID = 0x0800
FLAG_ROUTE_CODE = 0x0400
FLAG_TIMESTAMP = 0x0200
FLAG_RUNNABLE = 0x0100
FLAG_TAGS = 0x0080
FLAG_MIME_TYPE = 0x0020
FLAG_EOF = 0x0010
FLAG_FILE_CONTENT = 0x0040
EPOCH = datetime.datetime.utcfromtimestamp(0).replace(tzinfo=iso8601.Utc())
NUL_ELEMENT = b'\0'[0]
# Contains True for types for which 'nul in thing' falsely returns false.
_nul_test_broken = {}


def has_nul(buffer_or_bytes):
    """Return True if a null byte is present in buffer_or_bytes."""
    # Simple "if NUL_ELEMENT in utf8_bytes:" fails on Python 3.1 and 3.2 with
    # memoryviews. See https://bugs.launchpad.net/subunit/+bug/1216246
    buffer_type = type(buffer_or_bytes)
    broken = _nul_test_broken.get(buffer_type)
    if broken is None:
        reference = buffer_type(b'\0')
        broken = not NUL_ELEMENT in reference
        _nul_test_broken[buffer_type] = broken
    if broken:
        return b'\0' in buffer_or_bytes
    else:
        return NUL_ELEMENT in buffer_or_bytes


class ParseError(Exception):
    """Used to pass error messages within the parser."""


class StreamResultToBytes(object):
    """Convert StreamResult API calls to bytes.
    
    The StreamResult API is defined by testtools.StreamResult.
    """

    status_mask = {
        None: 0,
        'exists': 0x1,
        'inprogress': 0x2,
        'success': 0x3,
        'uxsuccess': 0x4,
        'skip': 0x5,
        'fail': 0x6,
        'xfail': 0x7,
        }

    zero_b = b'\0'[0]

    def __init__(self, output_stream):
        """Create a StreamResultToBytes with output written to output_stream.

        :param output_stream: A file-like object. Must support write(bytes)
            and flush() methods. Flush will be called after each write.
            The stream will be passed through subunit.make_stream_binary,
            to handle regular cases such as stdout.
        """
        self.output_stream = subunit.make_stream_binary(output_stream)

    def startTestRun(self):
        pass

    def stopTestRun(self):
        pass

    def status(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        self._write_packet(test_id=test_id, test_status=test_status,
            test_tags=test_tags, runnable=runnable, file_name=file_name,
            file_bytes=file_bytes, eof=eof, mime_type=mime_type,
            route_code=route_code, timestamp=timestamp)

    def _write_utf8(self, a_string, packet):
        utf8 = a_string.encode('utf-8')
        self._write_number(len(utf8), packet)
        packet.append(utf8)

    def _write_len16(self, length, packet):
        assert length < 65536
        packet.append(struct.pack(FMT_16, length))

    def _write_number(self, value, packet):
        packet.extend(self._encode_number(value))

    def _encode_number(self, value):
        assert value >= 0
        if value < 64:
            return [struct.pack(FMT_8, value)]
        elif value < 16384:
            value = value | 0x4000
            return [struct.pack(FMT_16, value)]
        elif value < 4194304:
            value = value | 0x800000
            return [struct.pack(FMT_16, value >> 8),
                    struct.pack(FMT_8, value & 0xff)]
        elif value < 1073741824:
            value = value | 0xc0000000
            return [struct.pack(FMT_32, value)]
        else:
            raise ValueError('value too large to encode: %r' % (value,))

    def _write_packet(self, test_id=None, test_status=None, test_tags=None,
        runnable=True, file_name=None, file_bytes=None, eof=False,
        mime_type=None, route_code=None, timestamp=None):
        packet = [SIGNATURE]
        packet.append(b'FF') # placeholder for flags
        # placeholder for length, but see below as length is variable.
        packet.append(b'')
        flags = 0x2000 # Version 0x2
        if timestamp is not None:
            flags = flags | FLAG_TIMESTAMP
            since_epoch = timestamp - EPOCH
            nanoseconds = since_epoch.microseconds * 1000
            seconds = (since_epoch.seconds + since_epoch.days * 24 * 3600)
            packet.append(struct.pack(FMT_32, seconds))
            self._write_number(nanoseconds, packet)
        if test_id is not None:
            flags = flags | FLAG_TEST_ID
            self._write_utf8(test_id, packet)
        if test_tags:
            flags = flags | FLAG_TAGS
            self._write_number(len(test_tags), packet)
            for tag in test_tags:
                self._write_utf8(tag, packet)
        if runnable:
            flags = flags | FLAG_RUNNABLE
        if mime_type:
            flags = flags | FLAG_MIME_TYPE
            self._write_utf8(mime_type, packet)
        if file_name is not None:
            flags = flags | FLAG_FILE_CONTENT
            self._write_utf8(file_name, packet)
            self._write_number(len(file_bytes), packet)
            packet.append(file_bytes)
        if eof: 
           flags = flags | FLAG_EOF
        if route_code is not None:
            flags = flags | FLAG_ROUTE_CODE
            self._write_utf8(route_code, packet)
        # 0x0008 - not used in v2.
        flags = flags | self.status_mask[test_status]
        packet[1] = struct.pack(FMT_16, flags)
        base_length = sum(map(len, packet)) + 4
        if base_length <= 62:
            # one byte to encode length, 62+1 = 63
            length_length = 1
        elif base_length <= 16381:
            # two bytes to encode length, 16381+2 = 16383
            length_length = 2
        elif base_length <= 4194300:
            # three bytes to encode length, 419430+3=4194303
            length_length = 3
        else:
            # Longer than policy:
            # TODO: chunk the packet automatically?
            # - strip all but file data
            # - do 4M chunks of that till done
            # - include original data in final chunk.
            raise ValueError("Length too long: %r" % base_length)
        packet[2:3] = self._encode_number(base_length + length_length)
        # We could either do a partial application of crc32 over each chunk
        # or a single join to a temp variable then a final join 
        # or two writes (that python might then split).
        # For now, simplest code: join, crc32, join, output
        content = b''.join(packet)
        self.output_stream.write(content + struct.pack(
            FMT_32, zlib.crc32(content) & 0xffffffff))
        self.output_stream.flush()


class ByteStreamToStreamResult(object):
    """Parse a subunit byte stream.

    Mixed streams that contain non-subunit content is supported when a
    non_subunit_name is passed to the contructor. The default is to raise an
    error containing the non-subunit byte after it has been read from the
    stream.

    Typical use:

       >>> case = ByteStreamToStreamResult(sys.stdin.buffer)
       >>> result = StreamResult()
       >>> result.startTestRun()
       >>> case.run(result)
       >>> result.stopTestRun()
    """

    status_lookup = {
        0x0: None,
        0x1: 'exists',
        0x2: 'inprogress',
        0x3: 'success',
        0x4: 'uxsuccess',
        0x5: 'skip',
        0x6: 'fail',
        0x7: 'xfail',
        }

    def __init__(self, source, non_subunit_name=None):
        """Create a ByteStreamToStreamResult.

        :param source: A file like object to read bytes from. Must support
            read(<count>) and return bytes. The file is not closed by
            ByteStreamToStreamResult. subunit.make_stream_binary() is
            called on the stream to get it into bytes mode.
        :param non_subunit_name: If set to non-None, non subunit content
            encountered in the stream will be converted into file packets
            labelled with this name.
        """
        self.non_subunit_name = non_subunit_name
        self.source = subunit.make_stream_binary(source)
        self.codec = codecs.lookup('utf8').incrementaldecoder()

    def run(self, result):
        """Parse source and emit events to result.
        
        This is a blocking call: it will run until EOF is detected on source.
        """
        self.codec.reset()
        mid_character = False
        while True:
            # We're in blocking mode; read one char
            content = self.source.read(1)
            if not content:
                # EOF
                return
            if not mid_character and content[0] == SIGNATURE[0]:
                self._parse_packet(result)
                continue
            if self.non_subunit_name is None:
                raise Exception("Non subunit content", content)
            try:
                if self.codec.decode(content):
                    # End of a character
                    mid_character = False
                else:
                    mid_character = True
            except UnicodeDecodeError:
                # Bad unicode, not our concern.
                mid_character = False
            # Aggregate all content that is not subunit until either
            # 1MiB is accumulated or 50ms has passed with no input.
            # Both are arbitrary amounts intended to give a simple
            # balance between efficiency (avoiding death by a thousand
            # one-byte packets), buffering (avoiding overlarge state
            # being hidden on intermediary nodes) and interactivity
            # (when driving a debugger, slow response to typing is
            # annoying).
            buffered = [content]
            while len(buffered[-1]):
                try:
                    self.source.fileno()
                except:
                    # Won't be able to select, fallback to
                    # one-byte-at-a-time.
                    break
                # Note: this has a very low timeout because with stdin, the
                # BufferedIO layer typically has all the content available
                # from the stream when e.g. pdb is dropped into, leading to
                # select always timing out when in fact we could have read
                # (from the buffer layer) - we typically fail to aggregate
                # any content on 3.x Pythons.
                readable = select.select([self.source], [], [], 0.000001)[0]
                if readable:
                    content = self.source.read(1)
                    if not len(content):
                        # EOF - break and emit buffered.
                        break
                    if not mid_character and content[0] == SIGNATURE[0]:
                        # New packet, break, emit buffered, then parse.
                        break
                    buffered.append(content)
                    # Feed into the codec.
                    try:
                        if self.codec.decode(content):
                            # End of a character
                            mid_character = False
                        else:
                            mid_character = True
                    except UnicodeDecodeError:
                        # Bad unicode, not our concern.
                        mid_character = False
                if not readable or len(buffered) >= 1048576:
                    # timeout or too much data, emit what we have.
                    break
            result.status(
                file_name=self.non_subunit_name,
                file_bytes=b''.join(buffered))
            if mid_character or not len(content) or content[0] != SIGNATURE[0]:
                continue
            # Otherwise, parse a data packet.
            self._parse_packet(result)

    def _parse_packet(self, result):
        try:
            packet = [SIGNATURE]
            self._parse(packet, result)
        except ParseError as error:
            result.status(test_id="subunit.parser", eof=True,
                file_name="Packet data", file_bytes=b''.join(packet),
                mime_type="application/octet-stream")
            result.status(test_id="subunit.parser", test_status='fail',
                eof=True, file_name="Parser Error",
                file_bytes=(error.args[0]).encode('utf8'),
                mime_type="text/plain;charset=utf8")

    def _to_bytes(self, data, pos, length):
        """Return a slice of data from pos for length as bytes."""
        # memoryview in 2.7.3 and 3.2 isn't directly usable with struct :(.
        # see https://bugs.launchpad.net/subunit/+bug/1216163
        result = data[pos:pos+length]
        if type(result) is not bytes:
            return result.tobytes()
        return result

    def _parse_varint(self, data, pos, max_3_bytes=False):
        # because the only incremental IO we do is at the start, and the 32 bit
        # CRC means we can always safely read enough to cover any varint, we
        # can be sure that there should be enough data - and if not it is an
        # error not a normal situation.
        data_0 = struct.unpack(FMT_8, self._to_bytes(data, pos, 1))[0]
        typeenum = data_0 & 0xc0
        value_0 = data_0 & 0x3f
        if typeenum == 0x00:
            return value_0, 1
        elif typeenum == 0x40:
            data_1 = struct.unpack(FMT_8, self._to_bytes(data, pos+1, 1))[0]
            return (value_0 << 8) | data_1, 2
        elif typeenum == 0x80:
            data_1 = struct.unpack(FMT_16, self._to_bytes(data, pos+1, 2))[0]
            return (value_0 << 16) | data_1, 3
        else:
            if max_3_bytes:
                raise ParseError('3 byte maximum given but 4 byte value found.')
            data_1, data_2 = struct.unpack(FMT_24, self._to_bytes(data, pos+1, 3))
            result = (value_0 << 24) | data_1 << 8 | data_2
            return result, 4

    def _parse(self, packet, result):
            # 2 bytes flags, at most 3 bytes length.
            packet.append(self.source.read(5))
            flags = struct.unpack(FMT_16, packet[-1][:2])[0]
            length, consumed = self._parse_varint(
                packet[-1], 2, max_3_bytes=True)
            remainder = self.source.read(length - 6)
            if len(remainder) != length - 6:
                raise ParseError(
                    'Short read - got %d bytes, wanted %d bytes' % (
                    len(remainder), length - 6))
            if consumed != 3:
                # Avoid having to parse torn values
                packet[-1] += remainder
                pos = 2 + consumed
            else:
                # Avoid copying potentially lots of data.
                packet.append(remainder)
                pos = 0
            crc = zlib.crc32(packet[0])
            for fragment in packet[1:-1]:
                crc = zlib.crc32(fragment, crc)
            crc = zlib.crc32(packet[-1][:-4], crc) & 0xffffffff
            packet_crc = struct.unpack(FMT_32, packet[-1][-4:])[0]
            if crc != packet_crc:
                # Bad CRC, report it and stop parsing the packet.
                raise ParseError(
                    'Bad checksum - calculated (0x%x), stored (0x%x)'
                        % (crc, packet_crc))
            if safe_hasattr(builtins, 'memoryview'):
                body = memoryview(packet[-1])
            else:
                body = packet[-1]
            # Discard CRC-32
            body = body[:-4]
            # One packet could have both file and status data; the Python API
            # presents these separately (perhaps it shouldn't?)
            if flags & FLAG_TIMESTAMP:
                seconds = struct.unpack(FMT_32, self._to_bytes(body, pos, 4))[0]
                nanoseconds, consumed = self._parse_varint(body, pos+4)
                pos = pos + 4 + consumed
                timestamp = EPOCH + datetime.timedelta(
                    seconds=seconds, microseconds=nanoseconds/1000)
            else:
                timestamp = None
            if flags & FLAG_TEST_ID:
                test_id, pos = self._read_utf8(body, pos)
            else:
                test_id = None
            if flags & FLAG_TAGS:
                tag_count, consumed = self._parse_varint(body, pos)
                pos += consumed
                test_tags = set()
                for _ in range(tag_count):
                    tag, pos = self._read_utf8(body, pos)
                    test_tags.add(tag)
            else:
                test_tags = None
            if flags & FLAG_MIME_TYPE:
                mime_type, pos = self._read_utf8(body, pos)
            else:
                mime_type = None
            if flags & FLAG_FILE_CONTENT:
                file_name, pos = self._read_utf8(body, pos)
                content_length, consumed = self._parse_varint(body, pos)
                pos += consumed
                file_bytes = self._to_bytes(body, pos, content_length)
                if len(file_bytes) != content_length:
                    raise ParseError('File content extends past end of packet: '
                        'claimed %d bytes, %d available' % (
                        content_length, len(file_bytes)))
                pos += content_length
            else:
                file_name = None
                file_bytes = None
            if flags & FLAG_ROUTE_CODE:
                route_code, pos = self._read_utf8(body, pos)
            else:
                route_code = None
            runnable = bool(flags & FLAG_RUNNABLE)
            eof = bool(flags & FLAG_EOF)
            test_status = self.status_lookup[flags & 0x0007]
            result.status(test_id=test_id, test_status=test_status,
                test_tags=test_tags, runnable=runnable, mime_type=mime_type,
                eof=eof, file_name=file_name, file_bytes=file_bytes,
                route_code=route_code, timestamp=timestamp)
    __call__ = run

    def _read_utf8(self, buf, pos):
        length, consumed = self._parse_varint(buf, pos)
        pos += consumed
        utf8_bytes = buf[pos:pos+length]
        if length != len(utf8_bytes):
            raise ParseError(
                'UTF8 string at offset %d extends past end of packet: '
                'claimed %d bytes, %d available' % (pos - 2, length,
                len(utf8_bytes)))
        if has_nul(utf8_bytes):
            raise ParseError('UTF8 string at offset %d contains NUL byte' % (
                pos-2,))
        try:
            utf8, decoded_bytes = utf_8_decode(utf8_bytes)
            if decoded_bytes != length:
                raise ParseError("Invalid (partially decodable) string at "
                    "offset %d, %d undecoded bytes" % (
                    pos-2, length - decoded_bytes))
            return utf8, length+pos
        except UnicodeDecodeError:
            raise ParseError('UTF8 string at offset %d is not UTF8' % (pos-2,))

