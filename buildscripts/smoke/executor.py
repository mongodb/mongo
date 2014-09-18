"""
Module which allows execution of a suite of tests with customizable fixtures and testers.

Fixtures are set up per-suite, and register APIs per-test.  Generally this is custom setup code.

Testers encapsulate test code of different types in a standard, UnitTest object.
"""

import inspect
import logging
import traceback
import unittest

import fixtures
import testers


def exec_suite(suite, logger, **kwargs):
    """Main entry point, executes a suite of tests with the given logger and executor arguments."""

    suite_executor = TestSuiteExecutor(logger, **kwargs)

    try:
        successful_setup = suite_executor.setup_suite(suite)

        if successful_setup:
            suite_executor.exec_suite()

    finally:
        suite_executor.teardown_suite(suite)


def instantiate(class_name, *args, **kwargs):
    """Helper to dynamically instantiate a class from a name."""
    split_name = class_name.split(".")
    module_name = split_name[0]
    class_name = ".".join(split_name[1:])

    module = __import__(module_name)
    class_ = getattr(module, class_name)
    return class_(*args, **kwargs)


class TestSuiteExecutor(object):

    """The state of execution of a suite of tests.

    The job of the TestSuiteExecutor is to convert the incoming fixtures and tester configuration
    into Fixture and TestCase objects, then execute them using the standard unittest framework.

    """

    def __init__(self, logger, testers={}, fixtures={}, fail_fast=False, **kwargs):

        self.logger = logger
        self.testers = testers
        self.fixtures = fixtures
        self.fail_fast = fail_fast

        if len(kwargs) > 0:
            raise optparse.OptionValueError("Unrecognized options for executor: %s" % kwargs)

        for fixture_name in self.fixtures:
            self.fixtures[fixture_name] = \
                self.build_fixture(fixture_name, **self.fixtures[fixture_name])

    def build_fixture(self, fixture_name, fixture_class=None, fixture_logger=None,
                      **fixture_kwargs):

        if not fixture_class:
            fixture_class = fixtures.DEFAULT_FIXTURE_CLASSES[fixture_name]

        if not fixture_logger:
            fixture_logger = self.logger.getChild("fixtures.%s" % fixture_name)
        else:
            fixture_logger = logging.getLogger(fixture_logger)

        return instantiate(fixture_class, fixture_logger, **fixture_kwargs)

    def build_tester(self, test):

        tester_type = test.test_type

        def extract_tester_args(tester_class=None, tester_logger=None, **tester_kwargs):
            return tester_class, tester_logger, tester_kwargs

        tester_class, tester_logger, tester_kwargs = \
            extract_tester_args(
                **(self.testers[tester_type] if tester_type in self.testers else {}))

        if not tester_class:
            tester_class = testers.DEFAULT_TESTER_CLASSES[tester_type]

        if not tester_logger:
            tester_logger = self.logger.getChild("testers.%s.%s" % (tester_type, test.uri))
        else:
            tester_logger = logging.getLogger(tester_logger)

        test_apis = []
        for fixture_name, fixture in self.fixtures.items():
            test_api = fixture.build_api(tester_type, tester_logger)
            if test_api:
                test_apis.append(test_api)

        return instantiate(tester_class, test, test_apis, tester_logger, **tester_kwargs)

    def setup_suite(self, suite):

        self.setup_fixtures = {}
        for fixture_name, fixture in self.fixtures.items():
            try:
                fixture.setup()
                self.setup_fixtures[fixture_name] = fixture
            except:
                print "Suite setup failed: %s" % fixture_name
                traceback.print_exc()
                return False

        self.unittest_suite = unittest.TestSuite()
        for test in suite:
            self.unittest_suite.addTest(self.build_tester(test))

        return True

    def exec_suite(self):
        # TODO: More stuff here?
        unittest.TextTestRunner(
            verbosity=2, failfast=self.fail_fast).run(self.unittest_suite)

    def teardown_suite(self, suite):

        for fixture_name, fixture in self.setup_fixtures.items():
            try:
                fixture.teardown()
            except:
                print "Suite teardown failed: %s" % fixture_name
                traceback.print_exc()
