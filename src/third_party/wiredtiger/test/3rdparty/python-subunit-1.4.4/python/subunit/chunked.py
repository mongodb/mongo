#
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2005  Robert Collins <robertc@robertcollins.net>
#  Copyright (C) 2011  Martin Pool <mbp@sourcefrog.net>
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

"""Encoder/decoder for http style chunked encoding."""

from testtools.compat import _b

empty = _b('')

class Decoder(object):
    """Decode chunked content to a byte stream."""

    def __init__(self, output, strict=True):
        """Create a decoder decoding to output.

        :param output: A file-like object. Bytes written to the Decoder are
            decoded to strip off the chunking and written to the output.
            Up to a full write worth of data or a single control line may be
            buffered (whichever is larger). The close method should be called
            when no more data is available, to detect short streams; the
            write method will return none-None when the end of a stream is
            detected. The output object must accept bytes objects.

        :param strict: If True (the default), the decoder will not knowingly
            accept input that is not conformant to the HTTP specification.
            (This does not imply that it will catch every nonconformance.)
            If False, it will accept incorrect input that is still
            unambiguous.
        """
        self.output = output
        self.buffered_bytes = []
        self.state = self._read_length
        self.body_length = 0
        self.strict = strict
        self._match_chars = _b("0123456789abcdefABCDEF\r\n")
        self._slash_n = _b('\n')
        self._slash_r = _b('\r')
        self._slash_rn = _b('\r\n')
        self._slash_nr = _b('\n\r')

    def close(self):
        """Close the decoder.

        :raises ValueError: If the stream is incomplete ValueError is raised.
        """
        if self.state != self._finished:
            raise ValueError("incomplete stream")

    def _finished(self):
        """Finished reading, return any remaining bytes."""
        if self.buffered_bytes:
            buffered_bytes = self.buffered_bytes
            self.buffered_bytes = []
            return empty.join(buffered_bytes)
        else:
            raise ValueError("stream is finished")

    def _read_body(self):
        """Pass body bytes to the output."""
        while self.body_length and self.buffered_bytes:
            if self.body_length >= len(self.buffered_bytes[0]):
                self.output.write(self.buffered_bytes[0])
                self.body_length -= len(self.buffered_bytes[0])
                del self.buffered_bytes[0]
                # No more data available.
                if not self.body_length:
                    self.state = self._read_length
            else:
                self.output.write(self.buffered_bytes[0][:self.body_length])
                self.buffered_bytes[0] = \
                    self.buffered_bytes[0][self.body_length:]
                self.body_length = 0
                self.state = self._read_length
                return self.state()

    def _read_length(self):
        """Try to decode a length from the bytes."""
        count_chars = []
        for bytes in self.buffered_bytes:
            for pos in range(len(bytes)):
                byte = bytes[pos:pos+1]
                if byte not in self._match_chars:
                    break
                count_chars.append(byte)
                if byte == self._slash_n:
                    break
        if not count_chars:
            return
        if count_chars[-1] != self._slash_n:
            return
        count_str = empty.join(count_chars)
        if self.strict:
            if count_str[-2:] != self._slash_rn:
                raise ValueError("chunk header invalid: %r" % count_str)
            if self._slash_r in count_str[:-2]:
                raise ValueError("too many CRs in chunk header %r" % count_str)
        self.body_length = int(count_str.rstrip(self._slash_nr), 16)
        excess_bytes = len(count_str)
        while excess_bytes:
            if excess_bytes >= len(self.buffered_bytes[0]):
                excess_bytes -= len(self.buffered_bytes[0])
                del self.buffered_bytes[0]
            else:
                self.buffered_bytes[0] = self.buffered_bytes[0][excess_bytes:]
                excess_bytes = 0
        if not self.body_length:
            self.state = self._finished
            if not self.buffered_bytes:
                # May not call into self._finished with no buffered data.
                return empty
        else:
            self.state = self._read_body
        return self.state()

    def write(self, bytes):
        """Decode bytes to the output stream.

        :raises ValueError: If the stream has already seen the end of file
            marker.
        :returns: None, or the excess bytes beyond the end of file marker.
        """
        if bytes:
            self.buffered_bytes.append(bytes)
        return self.state()


class Encoder(object):
    """Encode content to a stream using HTTP Chunked coding."""

    def __init__(self, output):
        """Create an encoder encoding to output.

        :param output: A file-like object. Bytes written to the Encoder
            will be encoded using HTTP chunking. Small writes may be buffered
            and the ``close`` method must be called to finish the stream.
        """
        self.output = output
        self.buffered_bytes = []
        self.buffer_size = 0

    def flush(self, extra_len=0):
        """Flush the encoder to the output stream.

        :param extra_len: Increase the size of the chunk by this many bytes
            to allow for a subsequent write.
        """
        if not self.buffer_size and not extra_len:
            return
        buffered_bytes = self.buffered_bytes
        buffer_size = self.buffer_size
        self.buffered_bytes = []
        self.buffer_size = 0
        self.output.write(_b("%X\r\n" % (buffer_size + extra_len)))
        if buffer_size:
            self.output.write(empty.join(buffered_bytes))
        return True

    def write(self, bytes):
        """Encode bytes to the output stream."""
        bytes_len = len(bytes)
        if self.buffer_size + bytes_len >= 65536:
            self.flush(bytes_len)
            self.output.write(bytes)
        else:
            self.buffered_bytes.append(bytes)
            self.buffer_size += bytes_len

    def close(self):
        """Finish the stream. This does not close the output stream."""
        self.flush()
        self.output.write(_b("0\r\n"))
