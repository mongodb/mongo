# Copyright (c) 2015 testtools developers. See LICENSE for details.

"""A collection of sample TestCases.

These are primarily of use in testing the test framework.
"""

from testscenarios import multiply_scenarios

from testtools import TestCase
from testtools.matchers import (
    AfterPreprocessing,
    Contains,
    Equals,
    MatchesDict,
    MatchesListwise,
)


def make_test_case(test_method_name, set_up=None, test_body=None,
                   tear_down=None, cleanups=(), pre_set_up=None,
                   post_tear_down=None):
    """Make a test case with the given behaviors.

    All callables are unary callables that receive this test as their argument.

    :param str test_method_name: The name of the test method.
    :param callable set_up: Implementation of setUp.
    :param callable test_body: Implementation of the actual test. Will be
        assigned to the test method.
    :param callable tear_down: Implementation of tearDown.
    :param cleanups: Iterable of callables that will be added as cleanups.
    :param callable pre_set_up: Called before the upcall to setUp().
    :param callable post_tear_down: Called after the upcall to tearDown().

    :return: A ``testtools.TestCase``.
    """
    set_up = set_up if set_up else _do_nothing
    test_body = test_body if test_body else _do_nothing
    tear_down = tear_down if tear_down else _do_nothing
    pre_set_up = pre_set_up if pre_set_up else _do_nothing
    post_tear_down = post_tear_down if post_tear_down else _do_nothing
    return _ConstructedTest(
        test_method_name, set_up, test_body, tear_down, cleanups,
        pre_set_up, post_tear_down,
    )


class _ConstructedTest(TestCase):
    """A test case defined by arguments, rather than overrides."""

    def __init__(self, test_method_name, set_up, test_body, tear_down,
                 cleanups, pre_set_up, post_tear_down):
        """Construct a test case.

        See ``make_test_case`` for full documentation.
        """
        setattr(self, test_method_name, self.test_case)
        super().__init__(test_method_name)
        self._set_up = set_up
        self._test_body = test_body
        self._tear_down = tear_down
        self._test_cleanups = cleanups
        self._pre_set_up = pre_set_up
        self._post_tear_down = post_tear_down

    def setUp(self):
        self._pre_set_up(self)
        super().setUp()
        for cleanup in self._test_cleanups:
            self.addCleanup(cleanup, self)
        self._set_up(self)

    def test_case(self):
        self._test_body(self)

    def tearDown(self):
        self._tear_down(self)
        super().tearDown()
        self._post_tear_down(self)


def _do_nothing(case):
    pass


_success = _do_nothing


def _error(case):
    1/0  # arbitrary non-failure exception


def _failure(case):
    case.fail('arbitrary failure')


def _skip(case):
    case.skipTest('arbitrary skip message')


def _expected_failure(case):
    case.expectFailure('arbitrary expected failure', _failure, case)


def _unexpected_success(case):
    case.expectFailure('arbitrary unexpected success', _success, case)


behaviors = [
    ('success', _success),
    ('fail', _failure),
    ('error',  _error),
    ('skip', _skip),
    ('xfail', _expected_failure),
    ('uxsuccess', _unexpected_success),
]


def _make_behavior_scenarios(stage):
    """Given a test stage, iterate over behavior scenarios for that stage.

    e.g.
        >>> list(_make_behavior_scenarios('set_up'))
        [('set_up=success', {'set_up_behavior': <function _success>}),
         ('set_up=fail', {'set_up_behavior': <function _failure>}),
         ('set_up=error', {'set_up_behavior': <function _error>}),
         ('set_up=skip', {'set_up_behavior': <function _skip>}),
         ('set_up=xfail', {'set_up_behavior': <function _expected_failure>),
         ('set_up=uxsuccess',
          {'set_up_behavior': <function _unexpected_success>})]

    Ordering is not consistent.
    """
    return (
        (f'{stage}={behavior}',
         {f'{stage}_behavior': function})
        for (behavior, function) in behaviors
    )


def make_case_for_behavior_scenario(case):
    """Given a test with a behavior scenario installed, make a TestCase."""
    cleanup_behavior = getattr(case, 'cleanup_behavior', None)
    cleanups = [cleanup_behavior] if cleanup_behavior else []
    return make_test_case(
        case.getUniqueString(),
        set_up=getattr(case, 'set_up_behavior', _do_nothing),
        test_body=getattr(case, 'body_behavior', _do_nothing),
        tear_down=getattr(case, 'tear_down_behavior', _do_nothing),
        cleanups=cleanups,
        pre_set_up=getattr(case, 'pre_set_up_behavior', _do_nothing),
        post_tear_down=getattr(case, 'post_tear_down_behavior', _do_nothing),
    )


class _SetUpFailsOnGlobalState(TestCase):
    """Fail to upcall setUp on first run. Fail to upcall tearDown after.

    This simulates a test that fails to upcall in ``setUp`` if some global
    state is broken, and fails to call ``tearDown`` when the global state
    breaks but works after that.
    """

    first_run = True

    def setUp(self):
        if not self.first_run:
            return
        super().setUp()

    def test_success(self):
        pass

    def tearDown(self):
        if not self.first_run:
            super().tearDown()
        self.__class__.first_run = False

    @classmethod
    def make_scenario(cls):
        case = cls('test_success')
        return {
            'case': case,
            'expected_first_result': _test_error_traceback(
                case, Contains('TestCase.tearDown was not called')),
            'expected_second_result': _test_error_traceback(
                case, Contains('TestCase.setUp was not called')),
        }


def _test_error_traceback(case, traceback_matcher):
    """Match result log of single test that errored out.

    ``traceback_matcher`` is applied to the text of the traceback.
    """
    return MatchesListwise([
        Equals(('startTest', case)),
        MatchesListwise([
            Equals('addError'),
            Equals(case),
            MatchesDict({
                'traceback': AfterPreprocessing(
                    lambda x: x.as_text(),
                    traceback_matcher,
                )
            })
        ]),
        Equals(('stopTest', case)),
    ])


"""
A list that can be used with testscenarios to test every deterministic sample
case that we have.
"""
deterministic_sample_cases_scenarios = multiply_scenarios(
    _make_behavior_scenarios('set_up'),
    _make_behavior_scenarios('body'),
    _make_behavior_scenarios('tear_down'),
    _make_behavior_scenarios('cleanup'),
) + [
    ('tear_down_fails_after_upcall', {
        'post_tear_down_behavior': _error,
    }),
]


"""
A list that can be used with testscenarios to test every non-deterministic
sample case that we have.
"""
nondeterministic_sample_cases_scenarios = [
    ('setup-fails-global-state', _SetUpFailsOnGlobalState.make_scenario()),
]
