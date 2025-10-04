"""The unittest.TestCase for mongos --test."""

from buildscripts.resmokelib import config, core, logging, utils
from buildscripts.resmokelib.testing.testcases import interface


class MongosTestCase(interface.ProcessTestCase):
    """A TestCase which runs a mongos binary with the given parameters."""

    REGISTERED_NAME = "mongos_test"

    def __init__(self, logger: logging.Logger, mongos_options: list[dict]):
        """Initialize the mongos test and saves the options."""

        assert len(mongos_options) == 1

        self.mongos_executable = utils.default_if_none(
            config.MONGOS_EXECUTABLE, config.DEFAULT_MONGOS_EXECUTABLE
        )
        # Use the executable as the test name.
        interface.ProcessTestCase.__init__(self, logger, "mongos test", self.mongos_executable)
        self.options = mongos_options[0].copy()

        self.process_kwargs = {}

    def configure(self, fixture, *args, **kwargs):
        """Ensure the --test option is present in the mongos options."""

        interface.ProcessTestCase.configure(self, fixture, *args, **kwargs)
        # Always specify test option to ensure the mongos will terminate.
        if "test" not in self.options:
            self.options["test"] = ""

        interface.append_process_tracking_options(self.process_kwargs, self._id)

    def _make_process(self):
        return core.programs.mongos_program(
            self.logger,
            self.fixture.job_num,
            executable=self.mongos_executable,
            mongos_options=self.options,
            process_kwargs=self.process_kwargs,
        )[0]
