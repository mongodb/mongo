# Copyright (c) 2011 testtools developers. See LICENSE for details.

from __future__ import with_statement

import sys

from testtools import (
    ExpectedException,
    TestCase,
    )

class TestExpectedException(TestCase):
    """Test the ExpectedException context manager."""

    def test_pass_on_raise(self):
        with ExpectedException(ValueError, 'tes.'):
            raise ValueError('test')

    def test_raise_on_text_mismatch(self):
        try:
            with ExpectedException(ValueError, 'tes.'):
                raise ValueError('mismatch')
        except AssertionError:
            e = sys.exc_info()[1]
            self.assertEqual('"mismatch" does not match "tes.".', str(e))
        else:
            self.fail('AssertionError not raised.')

    def test_raise_on_error_mismatch(self):
        try:
            with ExpectedException(TypeError, 'tes.'):
                raise ValueError('mismatch')
        except ValueError:
            e = sys.exc_info()[1]
            self.assertEqual('mismatch', str(e))
        else:
            self.fail('ValueError not raised.')

    def test_raise_if_no_exception(self):
        try:
            with ExpectedException(TypeError, 'tes.'):
                pass
        except AssertionError:
            e = sys.exc_info()[1]
            self.assertEqual('TypeError not raised.', str(e))
        else:
            self.fail('AssertionError not raised.')
