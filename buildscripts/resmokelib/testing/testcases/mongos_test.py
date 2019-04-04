"""The unittest.TestCase for merizos --test."""

from __future__ import absolute_import

from . import interface
from ... import config
from ... import core
from ... import utils


class MongosTestCase(interface.ProcessTestCase):
    """A TestCase which runs a merizos binary with the given parameters."""

    REGISTERED_NAME = "merizos_test"

    def __init__(self, logger, merizos_options):
        """Initialize the merizos test and saves the options."""

        self.merizos_executable = utils.default_if_none(config.MONGOS_EXECUTABLE,
                                                       config.DEFAULT_MONGOS_EXECUTABLE)
        # Use the executable as the test name.
        interface.ProcessTestCase.__init__(self, logger, "merizos test", self.merizos_executable)
        self.options = merizos_options.copy()

    def configure(self, fixture, *args, **kwargs):
        """Ensure the --test option is present in the merizos options."""

        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)
        # Always specify test option to ensure the merizos will terminate.
        if "test" not in self.options:
            self.options["test"] = ""

    def _make_process(self):
        return core.programs.merizos_program(self.logger, executable=self.merizos_executable,
                                            **self.options)
