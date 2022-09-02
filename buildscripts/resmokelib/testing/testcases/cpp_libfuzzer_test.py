"""The libfuzzertest.TestCase for C++ libfuzzer tests."""

import datetime
import os

from buildscripts.resmokelib import core
from buildscripts.resmokelib import utils
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
from buildscripts.resmokelib.testing.testcases import interface


class CPPLibfuzzerTestCase(interface.ProcessTestCase):
    """A C++ libfuzzer test to execute."""

    REGISTERED_NAME = "cpp_libfuzzer_test"
    DEFAULT_TIMEOUT = datetime.timedelta(hours=1)

    def __init__(self, logger, program_executable, program_options=None, runs=1000000,
                 corpus_directory_stem="corpora"):
        """Initialize the CPPLibfuzzerTestCase with the executable to run."""

        interface.ProcessTestCase.__init__(self, logger, "C++ libfuzzer test", program_executable)

        self.program_executable = program_executable
        self.program_options = utils.default_if_none(program_options, {}).copy()

        self.runs = runs

        self.corpus_directory = f"{corpus_directory_stem}/corpus-{self.short_name()}"
        self.merged_corpus_directory = f"{corpus_directory_stem}-merged/corpus-{self.short_name()}"

        os.makedirs(self.corpus_directory, exist_ok=True)

    def _make_process(self):
        default_args = [
            self.program_executable,
            "-max_len=100000",
            "-rss_limit_mb=5000",
            "-max_total_time=3600",  # 1 hour is the maximum amount of time to allow a fuzzer to run
            f"-runs={self.runs}",
            self.corpus_directory,
        ]
        return core.programs.make_process(self.logger, default_args, **self.program_options)
