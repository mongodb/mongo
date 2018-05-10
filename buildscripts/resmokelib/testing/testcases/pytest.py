"""The unittest.TestCase for Python unittests."""
import os
import unittest

from buildscripts.resmokelib.testing.testcases import interface


class PyTestCase(interface.TestCase):
    """A python test to execute."""

    REGISTERED_NAME = "py_test"

    def __init__(self, logger, py_filename):
        """Initialize PyTestCase."""
        interface.TestCase.__init__(self, logger, "PyTest", py_filename)

    def run_test(self):
        """Execute the test."""
        suite = unittest.defaultTestLoader.loadTestsFromName(self.test_module_name)
        result = unittest.TextTestRunner().run(suite)
        if not result.wasSuccessful():
            msg = "Python test {} failed".format(self.test_name)
            raise self.failureException(msg)

        self.return_code = 0

    def as_command(self):
        """Return execute command."""
        return "python -m unittest {}".format(self.test_module_name)

    @property
    def test_module_name(self):
        """Get the dotted module name from the path."""
        return os.path.splitext(self.test_name)[0].replace(os.sep, ".")
