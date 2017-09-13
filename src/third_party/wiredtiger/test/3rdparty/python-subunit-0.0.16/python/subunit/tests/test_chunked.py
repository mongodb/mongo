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

import unittest

from testtools.compat import _b, BytesIO

import subunit.chunked


class TestDecode(unittest.TestCase):

    def setUp(self):
        unittest.TestCase.setUp(self)
        self.output = BytesIO()
        self.decoder = subunit.chunked.Decoder(self.output)

    def test_close_read_length_short_errors(self):
        self.assertRaises(ValueError, self.decoder.close)

    def test_close_body_short_errors(self):
        self.assertEqual(None, self.decoder.write(_b('2\r\na')))
        self.assertRaises(ValueError, self.decoder.close)

    def test_close_body_buffered_data_errors(self):
        self.assertEqual(None, self.decoder.write(_b('2\r')))
        self.assertRaises(ValueError, self.decoder.close)

    def test_close_after_finished_stream_safe(self):
        self.assertEqual(None, self.decoder.write(_b('2\r\nab')))
        self.assertEqual(_b(''), self.decoder.write(_b('0\r\n')))
        self.decoder.close()

    def test_decode_nothing(self):
        self.assertEqual(_b(''), self.decoder.write(_b('0\r\n')))
        self.assertEqual(_b(''), self.output.getvalue())

    def test_decode_serialised_form(self):
        self.assertEqual(None, self.decoder.write(_b("F\r\n")))
        self.assertEqual(None, self.decoder.write(_b("serialised\n")))
        self.assertEqual(_b(''), self.decoder.write(_b("form0\r\n")))

    def test_decode_short(self):
        self.assertEqual(_b(''), self.decoder.write(_b('3\r\nabc0\r\n')))
        self.assertEqual(_b('abc'), self.output.getvalue())

    def test_decode_combines_short(self):
        self.assertEqual(_b(''), self.decoder.write(_b('6\r\nabcdef0\r\n')))
        self.assertEqual(_b('abcdef'), self.output.getvalue())

    def test_decode_excess_bytes_from_write(self):
        self.assertEqual(_b('1234'), self.decoder.write(_b('3\r\nabc0\r\n1234')))
        self.assertEqual(_b('abc'), self.output.getvalue())

    def test_decode_write_after_finished_errors(self):
        self.assertEqual(_b('1234'), self.decoder.write(_b('3\r\nabc0\r\n1234')))
        self.assertRaises(ValueError, self.decoder.write, _b(''))

    def test_decode_hex(self):
        self.assertEqual(_b(''), self.decoder.write(_b('A\r\n12345678900\r\n')))
        self.assertEqual(_b('1234567890'), self.output.getvalue())

    def test_decode_long_ranges(self):
        self.assertEqual(None, self.decoder.write(_b('10000\r\n')))
        self.assertEqual(None, self.decoder.write(_b('1' * 65536)))
        self.assertEqual(None, self.decoder.write(_b('10000\r\n')))
        self.assertEqual(None, self.decoder.write(_b('2' * 65536)))
        self.assertEqual(_b(''), self.decoder.write(_b('0\r\n')))
        self.assertEqual(_b('1' * 65536 + '2' * 65536), self.output.getvalue())

    def test_decode_newline_nonstrict(self):
        """Tolerate chunk markers with no CR character."""
        # From <http://pad.lv/505078>
        self.decoder = subunit.chunked.Decoder(self.output, strict=False)
        self.assertEqual(None, self.decoder.write(_b('a\n')))
        self.assertEqual(None, self.decoder.write(_b('abcdeabcde')))
        self.assertEqual(_b(''), self.decoder.write(_b('0\n')))
        self.assertEqual(_b('abcdeabcde'), self.output.getvalue())

    def test_decode_strict_newline_only(self):
        """Reject chunk markers with no CR character in strict mode."""
        # From <http://pad.lv/505078>
        self.assertRaises(ValueError,
            self.decoder.write, _b('a\n'))

    def test_decode_strict_multiple_crs(self):
        self.assertRaises(ValueError,
            self.decoder.write, _b('a\r\r\n'))

    def test_decode_short_header(self):
        self.assertRaises(ValueError,
            self.decoder.write, _b('\n'))


class TestEncode(unittest.TestCase):

    def setUp(self):
        unittest.TestCase.setUp(self)
        self.output = BytesIO()
        self.encoder = subunit.chunked.Encoder(self.output)

    def test_encode_nothing(self):
        self.encoder.close()
        self.assertEqual(_b('0\r\n'), self.output.getvalue())

    def test_encode_empty(self):
        self.encoder.write(_b(''))
        self.encoder.close()
        self.assertEqual(_b('0\r\n'), self.output.getvalue())

    def test_encode_short(self):
        self.encoder.write(_b('abc'))
        self.encoder.close()
        self.assertEqual(_b('3\r\nabc0\r\n'), self.output.getvalue())

    def test_encode_combines_short(self):
        self.encoder.write(_b('abc'))
        self.encoder.write(_b('def'))
        self.encoder.close()
        self.assertEqual(_b('6\r\nabcdef0\r\n'), self.output.getvalue())

    def test_encode_over_9_is_in_hex(self):
        self.encoder.write(_b('1234567890'))
        self.encoder.close()
        self.assertEqual(_b('A\r\n12345678900\r\n'), self.output.getvalue())

    def test_encode_long_ranges_not_combined(self):
        self.encoder.write(_b('1' * 65536))
        self.encoder.write(_b('2' * 65536))
        self.encoder.close()
        self.assertEqual(_b('10000\r\n' + '1' * 65536 + '10000\r\n' +
            '2' * 65536 + '0\r\n'), self.output.getvalue())
