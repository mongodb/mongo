# Copyright (c) 2008-2011 testtools developers. See LICENSE for details.

import os
import tempfile
import unittest

from testtools import TestCase
from testtools.compat import (
    _b,
    _u,
    StringIO,
    )
from testtools.content import (
    attach_file,
    Content,
    content_from_file,
    content_from_stream,
    TracebackContent,
    text_content,
    )
from testtools.content_type import (
    ContentType,
    UTF8_TEXT,
    )
from testtools.matchers import (
    Equals,
    MatchesException,
    Raises,
    raises,
    )
from testtools.tests.helpers import an_exc_info


raises_value_error = Raises(MatchesException(ValueError))


class TestContent(TestCase):

    def test___init___None_errors(self):
        self.assertThat(lambda: Content(None, None), raises_value_error)
        self.assertThat(
            lambda: Content(None, lambda: ["traceback"]), raises_value_error)
        self.assertThat(
            lambda: Content(ContentType("text", "traceback"), None),
            raises_value_error)

    def test___init___sets_ivars(self):
        content_type = ContentType("foo", "bar")
        content = Content(content_type, lambda: ["bytes"])
        self.assertEqual(content_type, content.content_type)
        self.assertEqual(["bytes"], list(content.iter_bytes()))

    def test___eq__(self):
        content_type = ContentType("foo", "bar")
        one_chunk = lambda: [_b("bytes")]
        two_chunk = lambda: [_b("by"), _b("tes")]
        content1 = Content(content_type, one_chunk)
        content2 = Content(content_type, one_chunk)
        content3 = Content(content_type, two_chunk)
        content4 = Content(content_type, lambda: [_b("by"), _b("te")])
        content5 = Content(ContentType("f", "b"), two_chunk)
        self.assertEqual(content1, content2)
        self.assertEqual(content1, content3)
        self.assertNotEqual(content1, content4)
        self.assertNotEqual(content1, content5)

    def test___repr__(self):
        content = Content(ContentType("application", "octet-stream"),
            lambda: [_b("\x00bin"), _b("ary\xff")])
        self.assertIn("\\x00binary\\xff", repr(content))

    def test_iter_text_not_text_errors(self):
        content_type = ContentType("foo", "bar")
        content = Content(content_type, lambda: ["bytes"])
        self.assertThat(content.iter_text, raises_value_error)

    def test_iter_text_decodes(self):
        content_type = ContentType("text", "strange", {"charset": "utf8"})
        content = Content(
            content_type, lambda: [_u("bytes\xea").encode("utf8")])
        self.assertEqual([_u("bytes\xea")], list(content.iter_text()))

    def test_iter_text_default_charset_iso_8859_1(self):
        content_type = ContentType("text", "strange")
        text = _u("bytes\xea")
        iso_version = text.encode("ISO-8859-1")
        content = Content(content_type, lambda: [iso_version])
        self.assertEqual([text], list(content.iter_text()))

    def test_from_file(self):
        fd, path = tempfile.mkstemp()
        self.addCleanup(os.remove, path)
        os.write(fd, _b('some data'))
        os.close(fd)
        content = content_from_file(path, UTF8_TEXT, chunk_size=2)
        self.assertThat(
            list(content.iter_bytes()),
            Equals([_b('so'), _b('me'), _b(' d'), _b('at'), _b('a')]))

    def test_from_nonexistent_file(self):
        directory = tempfile.mkdtemp()
        nonexistent = os.path.join(directory, 'nonexistent-file')
        content = content_from_file(nonexistent)
        self.assertThat(content.iter_bytes, raises(IOError))

    def test_from_file_default_type(self):
        content = content_from_file('/nonexistent/path')
        self.assertThat(content.content_type, Equals(UTF8_TEXT))

    def test_from_file_eager_loading(self):
        fd, path = tempfile.mkstemp()
        os.write(fd, _b('some data'))
        os.close(fd)
        content = content_from_file(path, UTF8_TEXT, buffer_now=True)
        os.remove(path)
        self.assertThat(
            ''.join(content.iter_text()), Equals('some data'))

    def test_from_stream(self):
        data = StringIO('some data')
        content = content_from_stream(data, UTF8_TEXT, chunk_size=2)
        self.assertThat(
            list(content.iter_bytes()), Equals(['so', 'me', ' d', 'at', 'a']))

    def test_from_stream_default_type(self):
        data = StringIO('some data')
        content = content_from_stream(data)
        self.assertThat(content.content_type, Equals(UTF8_TEXT))

    def test_from_stream_eager_loading(self):
        fd, path = tempfile.mkstemp()
        self.addCleanup(os.remove, path)
        os.write(fd, _b('some data'))
        stream = open(path, 'rb')
        content = content_from_stream(stream, UTF8_TEXT, buffer_now=True)
        os.write(fd, _b('more data'))
        os.close(fd)
        self.assertThat(
            ''.join(content.iter_text()), Equals('some data'))

    def test_from_text(self):
        data = _u("some data")
        expected = Content(UTF8_TEXT, lambda: [data.encode('utf8')])
        self.assertEqual(expected, text_content(data))


class TestTracebackContent(TestCase):

    def test___init___None_errors(self):
        self.assertThat(
            lambda: TracebackContent(None, None), raises_value_error)

    def test___init___sets_ivars(self):
        content = TracebackContent(an_exc_info, self)
        content_type = ContentType("text", "x-traceback",
            {"language": "python", "charset": "utf8"})
        self.assertEqual(content_type, content.content_type)
        result = unittest.TestResult()
        expected = result._exc_info_to_string(an_exc_info, self)
        self.assertEqual(expected, ''.join(list(content.iter_text())))


class TestAttachFile(TestCase):

    def make_file(self, data):
        # GZ 2011-04-21: This helper could be useful for methods above trying
        #                to use mkstemp, but should handle write failures and
        #                always close the fd. There must be a better way.
        fd, path = tempfile.mkstemp()
        self.addCleanup(os.remove, path)
        os.write(fd, _b(data))
        os.close(fd)
        return path

    def test_simple(self):
        class SomeTest(TestCase):
            def test_foo(self):
                pass
        test = SomeTest('test_foo')
        data = 'some data'
        path = self.make_file(data)
        my_content = text_content(data)
        attach_file(test, path, name='foo')
        self.assertEqual({'foo': my_content}, test.getDetails())

    def test_optional_name(self):
        # If no name is provided, attach_file just uses the base name of the
        # file.
        class SomeTest(TestCase):
            def test_foo(self):
                pass
        test = SomeTest('test_foo')
        path = self.make_file('some data')
        base_path = os.path.basename(path)
        attach_file(test, path)
        self.assertEqual([base_path], list(test.getDetails()))

    def test_lazy_read(self):
        class SomeTest(TestCase):
            def test_foo(self):
                pass
        test = SomeTest('test_foo')
        path = self.make_file('some data')
        attach_file(test, path, name='foo', buffer_now=False)
        content = test.getDetails()['foo']
        content_file = open(path, 'w')
        content_file.write('new data')
        content_file.close()
        self.assertEqual(''.join(content.iter_text()), 'new data')

    def test_eager_read_by_default(self):
        class SomeTest(TestCase):
            def test_foo(self):
                pass
        test = SomeTest('test_foo')
        path = self.make_file('some data')
        attach_file(test, path, name='foo')
        content = test.getDetails()['foo']
        content_file = open(path, 'w')
        content_file.write('new data')
        content_file.close()
        self.assertEqual(''.join(content.iter_text()), 'some data')


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
