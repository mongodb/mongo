"""The libfuzzertest.TestCase for C++ libfuzzer tests."""

import datetime
import os
import subprocess

from buildscripts.resmokelib import core
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.testcases import interface


class CPPLibfuzzerTestCase(interface.ProcessTestCase):
    """A C++ libfuzzer test to execute."""

    REGISTERED_NAME = "cpp_libfuzzer_test"
    DEFAULT_TIMEOUT = datetime.timedelta(hours=1)

    def __init__(self, logger, program_executable, program_options=None):
        """Initialize the CPPLibfuzzerTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "C++ libfuzzer test", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()
        self.corpus_directory = "corpus/corpus-" + self.short_name()

        os.makedirs(self.corpus_directory, exist_ok=True)

    def _make_process(self):
        default_args = [
            self.program_executable, self.corpus_directory, "-max_len=100000", "-rss_limit_mb=5000"
        ]
        self.program_options["job_num"] = self.fixture.job_num
        self.program_options["test_id"] = self._id
        return core.programs.make_process(self.logger, default_args, **self.program_options)

    def _execute(self, process):
        """Run the specified process."""
        self.logger.info("Starting Libfuzzer Test %s...\n%s", self.short_description(),
                         process.as_command())
        process.start()
        self.logger.info("%s started with pid %s.", self.short_description(), process.pid)
        try:
            self.return_code = process.wait(self.DEFAULT_TIMEOUT.total_seconds())
        except subprocess.TimeoutExpired:
            # If the test timeout, then no errors were detected. Thus, the return code should be 0.
            process.stop(mode=fixture_interface.TeardownMode.KILL)
            process.wait()
            self.logger.info("%s timed out. No errors were found.", self.short_description())
            self.return_code = 0

        if self.return_code != 0:
            self.logger.info("Failed %s", self.return_code)
            raise self.failureException("%s failed" % (self.short_description()))

        self.logger.info("%s finished.", self.short_description())
