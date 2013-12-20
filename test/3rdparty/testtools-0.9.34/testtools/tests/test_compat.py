# Copyright (c) 2010 testtools developers. See LICENSE for details.

"""Tests for miscellaneous compatibility functions"""

import io
import linecache
import os
import sys
import tempfile
import traceback

import testtools

from testtools.compat import (
    _b,
    _detect_encoding,
    _format_exc_info,
    _format_exception_only,
    _format_stack_list,
    _get_source_encoding,
    _u,
    reraise,
    str_is_unicode,
    text_repr,
    unicode_output_stream,
    )
from testtools.matchers import (
    Equals,
    Is,
    IsInstance,
    MatchesException,
    Not,
    Raises,
    )


class TestDetectEncoding(testtools.TestCase):
    """Test detection of Python source encodings"""

    def _check_encoding(self, expected, lines, possibly_invalid=False):
        """Check lines are valid Python and encoding is as expected"""
        if not possibly_invalid:
            compile(_b("".join(lines)), "<str>", "exec")
        encoding = _detect_encoding(lines)
        self.assertEqual(expected, encoding,
            "Encoding %r expected but got %r from lines %r" %
                (expected, encoding, lines))

    def test_examples_from_pep(self):
        """Check the examples given in PEP 263 all work as specified

        See 'Examples' section of <http://www.python.org/dev/peps/pep-0263/>
        """
        # With interpreter binary and using Emacs style file encoding comment:
        self._check_encoding("latin-1", (
            "#!/usr/bin/python\n",
            "# -*- coding: latin-1 -*-\n",
            "import os, sys\n"))
        self._check_encoding("iso-8859-15", (
            "#!/usr/bin/python\n",
            "# -*- coding: iso-8859-15 -*-\n",
            "import os, sys\n"))
        self._check_encoding("ascii", (
            "#!/usr/bin/python\n",
            "# -*- coding: ascii -*-\n",
            "import os, sys\n"))
        # Without interpreter line, using plain text:
        self._check_encoding("utf-8", (
            "# This Python file uses the following encoding: utf-8\n",
            "import os, sys\n"))
        # Text editors might have different ways of defining the file's
        # encoding, e.g.
        self._check_encoding("latin-1", (
            "#!/usr/local/bin/python\n",
            "# coding: latin-1\n",
            "import os, sys\n"))
        # Without encoding comment, Python's parser will assume ASCII text:
        self._check_encoding("ascii", (
            "#!/usr/local/bin/python\n",
            "import os, sys\n"))
        # Encoding comments which don't work:
        #   Missing "coding:" prefix:
        self._check_encoding("ascii", (
            "#!/usr/local/bin/python\n",
            "# latin-1\n",
            "import os, sys\n"))
        #   Encoding comment not on line 1 or 2:
        self._check_encoding("ascii", (
            "#!/usr/local/bin/python\n",
            "#\n",
            "# -*- coding: latin-1 -*-\n",
            "import os, sys\n"))
        #   Unsupported encoding:
        self._check_encoding("ascii", (
            "#!/usr/local/bin/python\n",
            "# -*- coding: utf-42 -*-\n",
            "import os, sys\n"),
            possibly_invalid=True)

    def test_bom(self):
        """Test the UTF-8 BOM counts as an encoding declaration"""
        self._check_encoding("utf-8", (
            "\xef\xbb\xbfimport sys\n",
            ))
        self._check_encoding("utf-8", (
            "\xef\xbb\xbf# File encoding: utf-8\n",
            ))
        self._check_encoding("utf-8", (
            '\xef\xbb\xbf"""Module docstring\n',
            '\xef\xbb\xbfThat should just be a ZWNB"""\n'))
        self._check_encoding("latin-1", (
            '"""Is this coding: latin-1 or coding: utf-8 instead?\n',
            '\xef\xbb\xbfThose should be latin-1 bytes"""\n'))
        self._check_encoding("utf-8", (
            "\xef\xbb\xbf# Is the coding: utf-8 or coding: euc-jp instead?\n",
            '"""Module docstring say \xe2\x98\x86"""\n'),
            possibly_invalid=True)

    def test_multiple_coding_comments(self):
        """Test only the first of multiple coding declarations counts"""
        self._check_encoding("iso-8859-1", (
            "# Is the coding: iso-8859-1\n",
            "# Or is it coding: iso-8859-2\n"),
            possibly_invalid=True)
        self._check_encoding("iso-8859-1", (
            "#!/usr/bin/python\n",
            "# Is the coding: iso-8859-1\n",
            "# Or is it coding: iso-8859-2\n"))
        self._check_encoding("iso-8859-1", (
            "# Is the coding: iso-8859-1 or coding: iso-8859-2\n",
            "# Or coding: iso-8859-3 or coding: iso-8859-4\n"),
            possibly_invalid=True)
        self._check_encoding("iso-8859-2", (
            "# Is the coding iso-8859-1 or coding: iso-8859-2\n",
            "# Spot the missing colon above\n"))


class TestGetSourceEncoding(testtools.TestCase):
    """Test reading and caching the encodings of source files"""

    def setUp(self):
        testtools.TestCase.setUp(self)
        dir = tempfile.mkdtemp()
        self.addCleanup(os.rmdir, dir)
        self.filename = os.path.join(dir, self.id().rsplit(".", 1)[1] + ".py")
        self._written = False

    def put_source(self, text):
        f = open(self.filename, "w")
        try:
            f.write(text)
        finally:
            f.close()
            if not self._written:
                self._written = True
                self.addCleanup(os.remove, self.filename)
                self.addCleanup(linecache.cache.pop, self.filename, None)

    def test_nonexistant_file_as_ascii(self):
        """When file can't be found, the encoding should default to ascii"""
        self.assertEquals("ascii", _get_source_encoding(self.filename))

    def test_encoding_is_cached(self):
        """The encoding should stay the same if the cache isn't invalidated"""
        self.put_source(
            "# coding: iso-8859-13\n"
            "import os\n")
        self.assertEquals("iso-8859-13", _get_source_encoding(self.filename))
        self.put_source(
            "# coding: rot-13\n"
            "vzcbeg bf\n")
        self.assertEquals("iso-8859-13", _get_source_encoding(self.filename))

    def test_traceback_rechecks_encoding(self):
        """A traceback function checks the cache and resets the encoding"""
        self.put_source(
            "# coding: iso-8859-8\n"
            "import os\n")
        self.assertEquals("iso-8859-8", _get_source_encoding(self.filename))
        self.put_source(
            "# coding: utf-8\n"
            "import os\n")
        try:
            exec (compile("raise RuntimeError\n", self.filename, "exec"))
        except RuntimeError:
            traceback.extract_tb(sys.exc_info()[2])
        else:
            self.fail("RuntimeError not raised")
        self.assertEquals("utf-8", _get_source_encoding(self.filename))


class _FakeOutputStream(object):
    """A simple file-like object for testing"""

    def __init__(self):
        self.writelog = []

    def write(self, obj):
        self.writelog.append(obj)


class TestUnicodeOutputStream(testtools.TestCase):
    """Test wrapping output streams so they work with arbitrary unicode"""

    uni = _u("pa\u026a\u03b8\u0259n")

    def setUp(self):
        super(TestUnicodeOutputStream, self).setUp()
        if sys.platform == "cli":
            self.skip("IronPython shouldn't wrap streams to do encoding")

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

    @testtools.skipIf(str_is_unicode, "Tests behaviour when str is not unicode")
    def test_unicode_encodings_wrapped_when_str_is_not_unicode(self):
        """A unicode encoding is wrapped but needs no error handler"""
        sout = _FakeOutputStream()
        sout.encoding = "utf-8"
        uout = unicode_output_stream(sout)
        self.assertEqual(uout.errors, "strict")
        uout.write(self.uni)
        self.assertEqual([_b("pa\xc9\xaa\xce\xb8\xc9\x99n")], sout.writelog)

    @testtools.skipIf(not str_is_unicode, "Tests behaviour when str is unicode")
    def test_unicode_encodings_not_wrapped_when_str_is_unicode(self):
        # No wrapping needed if native str type is unicode
        sout = _FakeOutputStream()
        sout.encoding = "utf-8"
        uout = unicode_output_stream(sout)
        self.assertIs(uout, sout)

    def test_stringio(self):
        """A StringIO object should maybe get an ascii native str type"""
        try:
            from cStringIO import StringIO
            newio = False
        except ImportError:
            from io import StringIO
            newio = True
        sout = StringIO()
        soutwrapper = unicode_output_stream(sout)
        soutwrapper.write(self.uni)
        if newio:
            self.assertEqual(self.uni, sout.getvalue())
        else:
            self.assertEqual("pa???n", sout.getvalue())

    def test_io_stringio(self):
        # io.StringIO only accepts unicode so should be returned as itself.
        s = io.StringIO()
        self.assertEqual(s, unicode_output_stream(s))

    def test_io_bytesio(self):
        # io.BytesIO only accepts bytes so should be wrapped.
        bytes_io = io.BytesIO()
        self.assertThat(bytes_io, Not(Is(unicode_output_stream(bytes_io))))
        # Will error if s was not wrapped properly.
        unicode_output_stream(bytes_io).write(_u('foo'))

    def test_io_textwrapper(self):
        # textwrapper is unicode, should be returned as itself.
        text_io = io.TextIOWrapper(io.BytesIO())
        self.assertThat(unicode_output_stream(text_io), Is(text_io))
        # To be sure...
        unicode_output_stream(text_io).write(_u('foo'))


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
        (_u("\x80"), "'\\x80'", "'''\\\n\\x80'''"),
        (_u("\x9F"), "'\\x9f'", "'''\\\n\\x9f'''"),
        # No-break space is unprintable
        (_u("\xA0"), "'\\xa0'", "'''\\\n\\xa0'''"),
        # Letters latin alphabets are printable
        (_u("\xA1"), _u("'\xa1'"), _u("'''\\\n\xa1'''")),
        (_u("\xFF"), _u("'\xff'"), _u("'''\\\n\xff'''")),
        (_u("\u0100"), _u("'\u0100'"), _u("'''\\\n\u0100'''")),
        # Line and paragraph seperators are unprintable
        (_u("\u2028"), "'\\u2028'", "'''\\\n\\u2028'''"),
        (_u("\u2029"), "'\\u2029'", "'''\\\n\\u2029'''"),
        # Unpaired surrogates are unprintable
        (_u("\uD800"), "'\\ud800'", "'''\\\n\\ud800'''"),
        (_u("\uDFFF"), "'\\udfff'", "'''\\\n\\udfff'''"),
        # Unprintable general categories not fully tested: Cc, Cf, Co, Cn, Zs
        )

    b_prefix = repr(_b(""))[:-2]
    u_prefix = repr(_u(""))[:-2]

    def test_ascii_examples_oneline_bytes(self):
        for s, expected, _ in self.ascii_examples:
            b = _b(s)
            actual = text_repr(b, multiline=False)
            # Add self.assertIsInstance check?
            self.assertEqual(actual, self.b_prefix + expected)
            self.assertEqual(eval(actual), b)

    def test_ascii_examples_oneline_unicode(self):
        for s, expected, _ in self.ascii_examples:
            u = _u(s)
            actual = text_repr(u, multiline=False)
            self.assertEqual(actual, self.u_prefix + expected)
            self.assertEqual(eval(actual), u)

    def test_ascii_examples_multiline_bytes(self):
        for s, _, expected in self.ascii_examples:
            b = _b(s)
            actual = text_repr(b, multiline=True)
            self.assertEqual(actual, self.b_prefix + expected)
            self.assertEqual(eval(actual), b)

    def test_ascii_examples_multiline_unicode(self):
        for s, _, expected in self.ascii_examples:
            u = _u(s)
            actual = text_repr(u, multiline=True)
            self.assertEqual(actual, self.u_prefix + expected)
            self.assertEqual(eval(actual), u)

    def test_ascii_examples_defaultline_bytes(self):
        for s, one, multi in self.ascii_examples:
            expected = "\n" in s and multi or one
            self.assertEqual(text_repr(_b(s)), self.b_prefix + expected)

    def test_ascii_examples_defaultline_unicode(self):
        for s, one, multi in self.ascii_examples:
            expected = "\n" in s and multi or one
            self.assertEqual(text_repr(_u(s)), self.u_prefix + expected)

    def test_bytes_examples_oneline(self):
        for b, expected, _ in self.bytes_examples:
            actual = text_repr(b, multiline=False)
            self.assertEqual(actual, self.b_prefix + expected)
            self.assertEqual(eval(actual), b)

    def test_bytes_examples_multiline(self):
        for b, _, expected in self.bytes_examples:
            actual = text_repr(b, multiline=True)
            self.assertEqual(actual, self.b_prefix + expected)
            self.assertEqual(eval(actual), b)

    def test_unicode_examples_oneline(self):
        for u, expected, _ in self.unicode_examples:
            actual = text_repr(u, multiline=False)
            self.assertEqual(actual, self.u_prefix + expected)
            self.assertEqual(eval(actual), u)

    def test_unicode_examples_multiline(self):
        for u, _, expected in self.unicode_examples:
            actual = text_repr(u, multiline=True)
            self.assertEqual(actual, self.u_prefix + expected)
            self.assertEqual(eval(actual), u)



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


class Python2CompatibilityTests(testtools.TestCase):

    def setUp(self):
        super(Python2CompatibilityTests, self).setUp()
        if sys.version[0] >= '3':
            self.skip("These tests are only applicable to python 2.")


class TestExceptionFormatting(Python2CompatibilityTests):
    """Test the _format_exception_only function."""

    def _assert_exception_format(self, eclass, evalue, expected):
        actual = _format_exception_only(eclass, evalue)
        self.assertThat(actual, Equals(expected))
        self.assertThat(''.join(actual), IsInstance(unicode))

    def test_supports_string_exception(self):
        self._assert_exception_format(
            "String_Exception",
            None,
            [_u("String_Exception\n")]
        )

    def test_supports_regular_exception(self):
        self._assert_exception_format(
            RuntimeError,
            RuntimeError("Something went wrong"),
            [_u("RuntimeError: Something went wrong\n")]
        )

    def test_supports_unprintable_exceptions(self):
        """Verify support for exception classes that raise an exception when
        __unicode__ or __str__ is called.
        """
        class UnprintableException(Exception):

            def __str__(self):
                raise Exception()

            def __unicode__(self):
                raise Exception()

        self._assert_exception_format(
            UnprintableException,
            UnprintableException("Foo"),
            [_u("UnprintableException: <unprintable UnprintableException object>\n")]
        )

    def test_supports_exceptions_with_no_string_value(self):
        class NoStringException(Exception):

            def __str__(self):
                return ""

            def __unicode__(self):
                return _u("")

        self._assert_exception_format(
            NoStringException,
            NoStringException("Foo"),
            [_u("NoStringException\n")]
        )

    def test_supports_strange_syntax_error(self):
        """Test support for syntax errors with unusual number of arguments"""
        self._assert_exception_format(
            SyntaxError,
            SyntaxError("Message"),
            [_u("SyntaxError: Message\n")]
        )

    def test_supports_syntax_error(self):
        self._assert_exception_format(
            SyntaxError,
            SyntaxError(
                "Some Syntax Message",
                (
                    "/path/to/file",
                    12,
                    2,
                    "This is the line of code",
                )
            ),
            [
                _u('  File "/path/to/file", line 12\n'),
                _u('    This is the line of code\n'),
                _u('     ^\n'),
                _u('SyntaxError: Some Syntax Message\n'),
            ]
        )


class StackListFormattingTests(Python2CompatibilityTests):
    """Test the _format_stack_list function."""

    def _assert_stack_format(self, stack_lines, expected_output):
        actual = _format_stack_list(stack_lines)
        self.assertThat(actual, Equals([expected_output]))

    def test_single_complete_stack_line(self):
        stack_lines = [(
            '/path/to/filename',
            12,
            'func_name',
            'some_code()',
        )]
        expected = \
            _u('  File "/path/to/filename", line 12, in func_name\n' \
               '    some_code()\n')

        self._assert_stack_format(stack_lines, expected)

    def test_single_stack_line_no_code(self):
        stack_lines = [(
            '/path/to/filename',
            12,
            'func_name',
            None
        )]
        expected = _u('  File "/path/to/filename", line 12, in func_name\n')
        self._assert_stack_format(stack_lines, expected)


class FormatExceptionInfoTests(Python2CompatibilityTests):

    def test_individual_functions_called(self):
        self.patch(
            testtools.compat,
            '_format_stack_list',
            lambda stack_list: [_u("format stack list called\n")]
        )
        self.patch(
            testtools.compat,
            '_format_exception_only',
            lambda etype, evalue: [_u("format exception only called\n")]
        )
        result = _format_exc_info(None, None, None)
        expected = [
            _u("Traceback (most recent call last):\n"),
            _u("format stack list called\n"),
            _u("format exception only called\n"),
        ]
        self.assertThat(expected, Equals(result))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
