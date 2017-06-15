"""
unittest.TestCase for C++ unit tests.
"""

from __future__ import absolute_import

from . import interface
from ... import core
from ... import utils


class CPPUnitTestCase(interface.TestCase):
    """
    A C++ unit test to execute.
    """

    REGISTERED_NAME = "cpp_unit_test"

    def __init__(self,
                 logger,
                 program_executable,
                 program_options=None):
        """
        Initializes the CPPUnitTestCase with the executable to run.
        """

        interface.TestCase.__init__(self, logger, "Program", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

    def run_test(self):
        try:
            program = self._make_process()
            self._execute(program)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running C++ unit test %s.", self.basename())
            raise

    def _make_process(self):
        return core.process.Process(self.logger,
                                    [self.program_executable],
                                    **self.program_options)
