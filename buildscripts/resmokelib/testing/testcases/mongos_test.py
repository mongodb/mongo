"""
unittest.TestCase for mongos --test.
"""

from __future__ import absolute_import

from . import interface
from ... import config
from ... import core
from ... import utils


class MongosTestCase(interface.TestCase):
    """
    A TestCase which runs a mongos binary with the given parameters.
    """

    REGISTERED_NAME = "mongos_test"

    def __init__(self,
                 logger,
                 mongos_options):
        """
        Initializes the mongos test and saves the options.
        """

        self.mongos_executable = utils.default_if_none(config.MONGOS_EXECUTABLE,
                                                       config.DEFAULT_MONGOS_EXECUTABLE)
        # Use the executable as the test name.
        interface.TestCase.__init__(self, logger, "mongos", self.mongos_executable)
        self.options = mongos_options.copy()

    def configure(self, fixture, *args, **kwargs):
        """
        Ensures the --test option is present in the mongos options.
        """

        interface.TestCase.configure(self, fixture, *args, **kwargs)
        # Always specify test option to ensure the mongos will terminate.
        if "test" not in self.options:
            self.options["test"] = ""

    def run_test(self):
        try:
            mongos = self._make_process()
            self._execute(mongos)
        except self.failureException:
            raise
        except:
            self.logger.exception("Encountered an error running %s.", mongos.as_command())
            raise

    def _make_process(self):
        return core.programs.mongos_program(self.logger,
                                            executable=self.mongos_executable,
                                            **self.options)
