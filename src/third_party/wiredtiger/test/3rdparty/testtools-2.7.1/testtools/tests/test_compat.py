# Copyright (c) 2010 testtools developers. See LICENSE for details.

"""Tests for miscellaneous compatibility functions"""

import ast
import io
import sys
import traceback

import testtools

from testtools.compat import (
    _b,
    reraise,
    text_repr,
    unicode_output_stream,
    )
from testtools.matchers import (
    Is,
    Not,
    )


class _FakeOutputStream:
    """A simple file-like object for testing"""

    def __init__(self):
        self.writelog = []

    def write(self, obj):
        self.writelog.append(obj)


class TestUnicodeOutputStream(testtools.TestCase):
    """Test wrapping output streams so they work with arbitrary unicode"""

    uni = "pa\u026a\u03b8\u0259n"

    def setUp(self):
        super().setUp()
        if sys.platform == "cli":
            self.skipTest("IronPython shouldn't wrap streams to do encoding")

    def test_no_encoding_becomes_ascii(self):
        """A stream with no encoding attribute gets ascii/replace strings"""
        sout = _FakeOutputStream()
        unicode_output_stream(sout).write(self.uni)
        self.assertEqual([_b("pa???n")], sout.writelog)

    def test_encoding_as_none_becomes_ascii(self):
        """A stream with encoding value of None gets ascii/replace strings"""
        sout = _FakeOutputStream()
        sout.encoding = None
        unicode_output_stream(sout).write(self.uni)
        self.assertEqual([_b("pa???n")], sout.writelog)

    def test_bogus_encoding_becomes_ascii(self):
        """A stream with a bogus encoding gets ascii/replace strings"""
        sout = _FakeOutputStream()
        sout.encoding = "bogus"
        unicode_output_stream(sout).write(self.uni)
        self.assertEqual([_b("pa???n")], sout.writelog)

    def test_partial_encoding_replace(self):
        """A string which can be partly encoded correctly should be"""
        sout = _FakeOutputStream()
        sout.encoding = "iso-8859-7"
        unicode_output_stream(sout).write(self.uni)
        self.assertEqual([_b("pa?\xe8?n")], sout.writelog)

    def test_stringio(self):
        """A StringIO object should maybe get an ascii native str type"""
        sout = io.StringIO()
        soutwrapper = unicode_output_stream(sout)
        soutwrapper.write(self.uni)
        self.assertEqual(self.uni, sout.getvalue())

    def test_io_stringio(self):
        # io.StringIO only accepts unicode so should be returned as itself.
        s = io.StringIO()
        self.assertEqual(s, unicode_output_stream(s))

    def test_io_bytesio(self):
        # io.BytesIO only accepts bytes so should be wrapped.
        bytes_io = io.BytesIO()
        self.assertThat(bytes_io, Not(Is(unicode_output_stream(bytes_io))))
        # Will error if s was not wrapped properly.
        unicode_output_stream(bytes_io).write('foo')

    def test_io_textwrapper(self):
        # textwrapper is unicode, should be returned as itself.
        text_io = io.TextIOWrapper(io.BytesIO())
        self.assertThat(unicode_output_stream(text_io), Is(text_io))
        # To be sure...
        unicode_output_stream(text_io).write('foo')


class TestTextRepr(testtools.TestCase):
    """Ensure in extending repr, basic behaviours are not being broken"""

    ascii_examples = (
        # Single character examples
        #  C0 control codes should be escaped except multiline \n
        ("\x00", "'\\x00'", "'''\\\n\\x00'''"),
        ("\b", "'\\x08'", "'''\\\n\\x08'''"),
        ("\t", "'\\t'", "'''\\\n\\t'''"),
        ("\n", "'\\n'", "'''\\\n\n'''"),
        ("\r", "'\\r'", "'''\\\n\\r'''"),
        #  Quotes and backslash should match normal repr behaviour
        ('"', "'\"'", "'''\\\n\"'''"),
        ("'", "\"'\"", "'''\\\n\\''''"),
        ("\\", "'\\\\'", "'''\\\n\\\\'''"),
        #  DEL is also unprintable and should be escaped
        ("\x7F", "'\\x7f'", "'''\\\n\\x7f'''"),

        # Character combinations that need double checking
        ("\r\n", "'\\r\\n'", "'''\\\n\\r\n'''"),
        ("\"'", "'\"\\''", "'''\\\n\"\\''''"),
        ("'\"", "'\\'\"'", "'''\\\n'\"'''"),
        ("\\n", "'\\\\n'", "'''\\\n\\\\n'''"),
        ("\\\n", "'\\\\\\n'", "'''\\\n\\\\\n'''"),
        ("\\' ", "\"\\\\' \"", "'''\\\n\\\\' '''"),
        ("\\'\n", "\"\\\\'\\n\"", "'''\\\n\\\\'\n'''"),
        ("\\'\"", "'\\\\\\'\"'", "'''\\\n\\\\'\"'''"),
        ("\\'''", "\"\\\\'''\"", "'''\\\n\\\\\\'\\'\\''''"),
        )

    # Bytes with the high bit set should always be escaped
    bytes_examples = (
        (_b("\x80"), "'\\x80'", "'''\\\n\\x80'''"),
        (_b("\xA0"), "'\\xa0'", "'''\\\n\\xa0'''"),
        (_b("\xC0"), "'\\xc0'", "'''\\\n\\xc0'''"),
        (_b("\xFF"), "'\\xff'", "'''\\\n\\xff'''"),
        (_b("\xC2\xA7"), "'\\xc2\\xa7'", "'''\\\n\\xc2\\xa7'''"),
        )

    # Unicode doesn't escape printable characters as per the Python 3 model
    unicode_examples = (
        # C1 codes are unprintable
        ("\x80", "'\\x80'", "'''\\\n\\x80'''"),
        ("\x9F", "'\\x9f'", "'''\\\n\\x9f'''"),
        # No-break space is unprintable
        ("\xA0", "'\\xa0'", "'''\\\n\\xa0'''"),
        # Letters latin alphabets are printable
        ("\xA1", "'\xa1'", "'''\\\n\xa1'''"),
        ("\xFF", "'\xff'", "'''\\\n\xff'''"),
        ("\u0100", "'\u0100'", "'''\\\n\u0100'''"),
        # Line and paragraph seperators are unprintable
        ("\u2028", "'\\u2028'", "'''\\\n\\u2028'''"),
        ("\u2029", "'\\u2029'", "'''\\\n\\u2029'''"),
        # Unpaired surrogates are unprintable
        ("\uD800", "'\\ud800'", "'''\\\n\\ud800'''"),
        ("\uDFFF", "'\\udfff'", "'''\\\n\\udfff'''"),
        # Unprintable general categories not fully tested: Cc, Cf, Co, Cn, Zs
        )

    b_prefix = repr(_b(""))[:-2]
    u_prefix = repr("")[:-2]

    def test_ascii_examples_oneline_bytes(self):
        for s, expected, _ in self.ascii_examples:
            b = _b(s)
            actual = text_repr(b, multiline=False)
            # Add self.assertIsInstance check?
            self.assertEqual(actual, self.b_prefix + expected)
            self.assertEqual(ast.literal_eval(actual), b)

    def test_ascii_examples_oneline_unicode(self):
        for s, expected, _ in self.ascii_examples:
            u = s
            actual = text_repr(u, multiline=False)
            self.assertEqual(actual, self.u_prefix + expected)
            self.assertEqual(ast.literal_eval(actual), u)

    def test_ascii_examples_multiline_bytes(self):
        for s, _, expected in self.ascii_examples:
            b = _b(s)
            actual = text_repr(b, multiline=True)
            self.assertEqual(actual, self.b_prefix + expected)
            self.assertEqual(ast.literal_eval(actual), b)

    def test_ascii_examples_multiline_unicode(self):
        for s, _, expected in self.ascii_examples:
            actual = text_repr(s, multiline=True)
            self.assertEqual(actual, self.u_prefix + expected)
            self.assertEqual(ast.literal_eval(actual), s)

    def test_ascii_examples_defaultline_bytes(self):
        for s, one, multi in self.ascii_examples:
            expected = "\n" in s and multi or one
            self.assertEqual(text_repr(_b(s)), self.b_prefix + expected)

    def test_ascii_examples_defaultline_unicode(self):
        for s, one, multi in self.ascii_examples:
            expected = "\n" in s and multi or one
            self.assertEqual(text_repr(s), self.u_prefix + expected)

    def test_bytes_examples_oneline(self):
        for b, expected, _ in self.bytes_examples:
            actual = text_repr(b, multiline=False)
            self.assertEqual(actual, self.b_prefix + expected)
            self.assertEqual(ast.literal_eval(actual), b)

    def test_bytes_examples_multiline(self):
        for b, _, expected in self.bytes_examples:
            actual = text_repr(b, multiline=True)
            self.assertEqual(actual, self.b_prefix + expected)
            self.assertEqual(ast.literal_eval(actual), b)

    def test_unicode_examples_oneline(self):
        for u, expected, _ in self.unicode_examples:
            actual = text_repr(u, multiline=False)
            self.assertEqual(actual, self.u_prefix + expected)
            self.assertEqual(ast.literal_eval(actual), u)

    def test_unicode_examples_multiline(self):
        for u, _, expected in self.unicode_examples:
            actual = text_repr(u, multiline=True)
            self.assertEqual(actual, self.u_prefix + expected)
            self.assertEqual(ast.literal_eval(actual), u)



class TestReraise(testtools.TestCase):
    """Tests for trivial reraise wrapper needed for Python 2/3 changes"""

    def test_exc_info(self):
        """After reraise exc_info matches plus some extra traceback"""
        try:
            raise ValueError("Bad value")
        except ValueError:
            _exc_info = sys.exc_info()
        try:
            reraise(*_exc_info)
        except ValueError:
            _new_exc_info = sys.exc_info()
        self.assertIs(_exc_info[0], _new_exc_info[0])
        self.assertIs(_exc_info[1], _new_exc_info[1])
        expected_tb = traceback.extract_tb(_exc_info[2])
        self.assertEqual(expected_tb,
            traceback.extract_tb(_new_exc_info[2])[-len(expected_tb):])

    def test_custom_exception_no_args(self):
        """Reraising does not require args attribute to contain params"""

        class CustomException(Exception):
            """Exception that expects and sets attrs but not args"""

            def __init__(self, value):
                Exception.__init__(self)
                self.value = value

        try:
            raise CustomException("Some value")
        except CustomException:
            _exc_info = sys.exc_info()
        self.assertRaises(CustomException, reraise, *_exc_info)


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
