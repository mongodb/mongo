# Copyright (c) 2008-2016 testtools developers. See LICENSE for details.

import warnings

from testtools import TestCase
from testtools.matchers import (
    AfterPreprocessing,
    Equals,
    MatchesStructure,
    MatchesListwise,
    Contains,
    HasLength,
    )
from testtools.matchers._warnings import Warnings, IsDeprecated, WarningMessage
from testtools.tests.helpers import FullStackRunTest
from testtools.tests.matchers.helpers import TestMatchersInterface


def make_warning(warning_type, message):
    warnings.warn(message, warning_type, 2)


def make_warning_message(message, category, filename=None, lineno=None, line=None):
    return warnings.WarningMessage(
        message=message,
        category=category,
        filename=filename,
        lineno=lineno,
        line=line)


class TestWarningMessageCategoryTypeInterface(TestCase, TestMatchersInterface):
    """
    Tests for `testtools.matchers._warnings.WarningMessage`.

    In particular matching the ``category_type``.
    """
    matches_matcher = WarningMessage(category_type=DeprecationWarning)
    warning_foo = make_warning_message('foo', DeprecationWarning)
    warning_bar = make_warning_message('bar', SyntaxWarning)
    warning_base = make_warning_message('base', Warning)
    matches_matches = [warning_foo]
    matches_mismatches = [warning_bar, warning_base]

    str_examples = []
    describe_examples = []


class TestWarningMessageMessageInterface(TestCase, TestMatchersInterface):
    """
    Tests for `testtools.matchers._warnings.WarningMessage`.

    In particular matching the ``message``.
    """
    matches_matcher = WarningMessage(category_type=DeprecationWarning,
                                     message=Equals('foo'))
    warning_foo = make_warning_message('foo', DeprecationWarning)
    warning_bar = make_warning_message('bar', DeprecationWarning)
    matches_matches = [warning_foo]
    matches_mismatches = [warning_bar]

    str_examples = []
    describe_examples = []


class TestWarningMessageFilenameInterface(TestCase, TestMatchersInterface):
    """
    Tests for `testtools.matchers._warnings.WarningMessage`.

    In particular matching the ``filename``.
    """
    matches_matcher = WarningMessage(category_type=DeprecationWarning,
                                     filename=Equals('a'))
    warning_foo = make_warning_message('foo', DeprecationWarning, filename='a')
    warning_bar = make_warning_message('bar', DeprecationWarning, filename='b')
    matches_matches = [warning_foo]
    matches_mismatches = [warning_bar]

    str_examples = []
    describe_examples = []


class TestWarningMessageLineNumberInterface(TestCase, TestMatchersInterface):
    """
    Tests for `testtools.matchers._warnings.WarningMessage`.

    In particular matching the ``lineno``.
    """
    matches_matcher = WarningMessage(category_type=DeprecationWarning,
                                     lineno=Equals(42))
    warning_foo = make_warning_message('foo', DeprecationWarning, lineno=42)
    warning_bar = make_warning_message('bar', DeprecationWarning, lineno=21)
    matches_matches = [warning_foo]
    matches_mismatches = [warning_bar]

    str_examples = []
    describe_examples = []


class TestWarningMessageLineInterface(TestCase, TestMatchersInterface):
    """
    Tests for `testtools.matchers._warnings.WarningMessage`.

    In particular matching the ``line``.
    """
    matches_matcher = WarningMessage(category_type=DeprecationWarning,
                                     line=Equals('x'))
    warning_foo = make_warning_message('foo', DeprecationWarning, line='x')
    warning_bar = make_warning_message('bar', DeprecationWarning, line='y')
    matches_matches = [warning_foo]
    matches_mismatches = [warning_bar]

    str_examples = []
    describe_examples = []


class TestWarningsInterface(TestCase, TestMatchersInterface):
    """
    Tests for `testtools.matchers._warnings.Warnings`.

    Specifically without the optional argument.
    """
    matches_matcher = Warnings()
    def old_func():
        warnings.warn('old_func is deprecated', DeprecationWarning, 2)
    matches_matches = [old_func]
    matches_mismatches = [lambda: None]

    # Tricky to get function objects to render constantly, and the interfaces
    # helper uses assertEqual rather than (for instance) DocTestMatches.
    str_examples = []

    describe_examples = []


class TestWarningsMatcherInterface(TestCase, TestMatchersInterface):
    """
    Tests for `testtools.matchers._warnings.Warnings`.

    Specifically with the optional matcher argument.
    """
    matches_matcher = Warnings(
        warnings_matcher=MatchesListwise([
            MatchesStructure(
                message=AfterPreprocessing(
                    str, Contains('old_func')))]))
    def old_func():
        warnings.warn('old_func is deprecated', DeprecationWarning, 2)
    def older_func():
        warnings.warn('older_func is deprecated', DeprecationWarning, 2)
    matches_matches = [old_func]
    matches_mismatches = [lambda:None, older_func]

    str_examples = []
    describe_examples = []


class TestWarningsMatcherNoWarningsInterface(TestCase, TestMatchersInterface):
    """
    Tests for `testtools.matchers._warnings.Warnings`.

    Specifically with the optional matcher argument matching that there were no
    warnings.
    """
    matches_matcher = Warnings(warnings_matcher=HasLength(0))
    def nowarning_func():
        pass
    def warning_func():
        warnings.warn('warning_func is deprecated', DeprecationWarning, 2)
    matches_matches = [nowarning_func]
    matches_mismatches = [warning_func]

    str_examples = []
    describe_examples = []


class TestWarningMessage(TestCase):
    """
    Tests for `testtools.matchers._warnings.WarningMessage`.
    """
    run_tests_with = FullStackRunTest

    def test_category(self):
        def old_func():
            warnings.warn('old_func is deprecated', DeprecationWarning, 2)
        self.assertThat(old_func, IsDeprecated(Contains('old_func')))


class TestIsDeprecated(TestCase):
    """
    Tests for `testtools.matchers._warnings.IsDeprecated`.
    """
    run_tests_with = FullStackRunTest

    def test_warning(self):
        def old_func():
            warnings.warn('old_func is deprecated', DeprecationWarning, 2)
        self.assertThat(old_func, IsDeprecated(Contains('old_func')))


def test_suite():
    from unittest import TestLoader
    return TestLoader().loadTestsFromName(__name__)
