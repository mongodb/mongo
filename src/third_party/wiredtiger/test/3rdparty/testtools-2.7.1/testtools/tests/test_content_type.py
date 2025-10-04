# Copyright (c) 2008, 2012 testtools developers. See LICENSE for details.

from testtools import TestCase
from testtools.matchers import Equals, MatchesException, Raises
from testtools.content_type import (
    ContentType,
    JSON,
    UTF8_TEXT,
    )


class TestContentType(TestCase):

    def test___init___None_errors(self):
        raises_value_error = Raises(MatchesException(ValueError))
        self.assertThat(lambda:ContentType(None, None), raises_value_error)
        self.assertThat(lambda:ContentType(None, "traceback"),
            raises_value_error)
        self.assertThat(lambda:ContentType("text", None), raises_value_error)

    def test___init___sets_ivars(self):
        content_type = ContentType("foo", "bar")
        self.assertEqual("foo", content_type.type)
        self.assertEqual("bar", content_type.subtype)
        self.assertEqual({}, content_type.parameters)

    def test___init___with_parameters(self):
        content_type = ContentType("foo", "bar", {"quux": "thing"})
        self.assertEqual({"quux": "thing"}, content_type.parameters)

    def test___eq__(self):
        content_type1 = ContentType("foo", "bar", {"quux": "thing"})
        content_type2 = ContentType("foo", "bar", {"quux": "thing"})
        content_type3 = ContentType("foo", "bar", {"quux": "thing2"})
        self.assertTrue(content_type1.__eq__(content_type2))
        self.assertFalse(content_type1.__eq__(content_type3))

    def test_basic_repr(self):
        content_type = ContentType('text', 'plain')
        self.assertThat(repr(content_type), Equals('text/plain'))

    def test_extended_repr(self):
        content_type = ContentType(
            'text', 'plain', {'foo': 'bar', 'baz': 'qux'})
        self.assertThat(
            repr(content_type), Equals('text/plain; baz="qux"; foo="bar"'))


class TestBuiltinContentTypes(TestCase):

    def test_plain_text(self):
        # The UTF8_TEXT content type represents UTF-8 encoded text/plain.
        self.assertThat(UTF8_TEXT.type, Equals('text'))
        self.assertThat(UTF8_TEXT.subtype, Equals('plain'))
        self.assertThat(UTF8_TEXT.parameters, Equals({'charset': 'utf8'}))

    def test_json_content(self):
        # The JSON content type represents implictly UTF-8 application/json.
        self.assertThat(JSON.type, Equals('application'))
        self.assertThat(JSON.subtype, Equals('json'))
        self.assertThat(JSON.parameters, Equals({}))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
