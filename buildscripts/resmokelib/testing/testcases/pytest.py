"""
unittest.TestCase for Python unittests.
"""
import os
import unittest

from buildscripts.resmokelib.testing.testcases import interface


class PyTestCase(interface.TestCase):

    REGISTERED_NAME = "py_test"

    def __init__(self, logger, py_filename):
        interface.TestCase.__init__(self, logger, "PyTest", py_filename)

    def run_test(self):
        suite = unittest.defaultTestLoader.loadTestsFromName(self.test_module_name)
        result = unittest.TextTestRunner().run(suite)
        if result.failures:
            msg = "Python test {} failed".format(self.test_name)
            raise self.failureException(msg)

        self.return_code = 0

    def as_command(self):
        return "python -m unittest {}".format(self.test_module_name)

    @property
    def test_module_name(self):
        """Get the dotted module name from the path."""
        return os.path.splitext(self.test_name)[0].replace(os.sep, ".")
