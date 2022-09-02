"""Unit tests for the resmokelib.testing.fixtures.interface module."""
import logging
import unittest

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.fixtures import interface
from buildscripts.resmokelib.testing.fixtures.fixturelib import FixtureLib


class TestFixture(unittest.TestCase):
    def test_teardown_ok(self):
        raising_fixture = UnitTestFixture(should_raise=False)
        raising_fixture.teardown()

    def test_teardown_raise(self):
        raising_fixture = UnitTestFixture(should_raise=True)
        with self.assertRaises(errors.ServerFailure):
            raising_fixture.teardown()


class TestFixtureTeardownHandler(unittest.TestCase):
    def test_teardown_ok(self):
        handler = interface.FixtureTeardownHandler(logging.getLogger("handler_unittests"))
        # Before any teardown.
        self.assertTrue(handler.was_successful())
        self.assertIsNone(handler.get_error_message())
        # Successful teardown.
        ok_fixture = UnitTestFixture(should_raise=False)
        handler.teardown(ok_fixture, "ok")
        # After successful teardown.
        self.assertTrue(handler.was_successful())
        self.assertIsNone(handler.get_error_message())

    def test_teardown_error(self):
        handler = interface.FixtureTeardownHandler(logging.getLogger("handler_unittests"))
        # Failing teardown.
        ko_fixture = UnitTestFixture(should_raise=True)
        handler.teardown(ko_fixture, "ko")
        # After failed teardown.
        self.assertFalse(handler.was_successful())
        expected_msg = "Error while stopping ko: " + UnitTestFixture.ERROR_MESSAGE
        self.assertEqual(expected_msg, handler.get_error_message())


class UnitTestFixture(interface.Fixture):  # pylint: disable=abstract-method
    ERROR_MESSAGE = "Failed"

    def __init__(self, should_raise=False):
        logger = logging.getLogger("fixture_unittests")
        fixturelib = FixtureLib()
        interface.Fixture.__init__(self, logger, 99, fixturelib)
        self._should_raise = should_raise

    def _do_teardown(self, mode=None):
        if self._should_raise:
            raise errors.ServerFailure(self.ERROR_MESSAGE)
