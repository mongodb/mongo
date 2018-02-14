"""
unittest.TestCase for tests using a MongoDB vendored version of Google Benchmark.
"""

from __future__ import absolute_import

from . import interface
from ... import core
from ... import utils


class BenchmarkTestCase(interface.TestCase):
    """
    A Benchmark test to execute.
    """

    REGISTERED_NAME = "benchmark_test"

    def __init__(self,
                 logger,
                 program_executable,
                 program_options=None):
        """
        Initializes the BenchmarkTestCase with the executable to run.
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
            self.logger.exception(
                "Encountered an error running Benchmark test %s.", self.basename())
            raise

    def _make_process(self):
        return core.programs.generic_program(self.logger,
                                             [self.program_executable],
                                             **self.program_options)
