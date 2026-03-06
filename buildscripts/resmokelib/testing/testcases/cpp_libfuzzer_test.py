"""The libfuzzertest.TestCase for C++ libfuzzer tests."""

import datetime
import os
from typing import Optional

from buildscripts.resmokelib import core, logging, utils
from buildscripts.resmokelib.testing.testcases import interface


class CPPLibfuzzerTestCase(interface.ProcessTestCase):
    """A C++ libfuzzer test to execute."""

    REGISTERED_NAME = "cpp_libfuzzer_test"
    DEFAULT_TIMEOUT = datetime.timedelta(hours=1)

    def __init__(
        self,
        logger: logging.Logger,
        program_executables: list[str],
        program_options: Optional[dict] = None,
        corpus_directory_stem="corpora",
    ):
        """Initialize the CPPLibfuzzerTestCase with the executable to run."""

        assert len(program_executables) == 1
        interface.ProcessTestCase.__init__(
            self, logger, "C++ libfuzzer test", program_executables[0]
        )

        self.program_executable = program_executables[0]
        self.program_options = utils.default_if_none(program_options, {}).copy()

        # disable writing of profraw during fuzzing unless specifically asked for
        self.program_options.setdefault("env_vars", {})
        self.program_options["env_vars"].setdefault("LLVM_PROFILE_FILE", "/dev/null")

        self.corpus_directory = corpus_directory_stem
        self.seed_directory = os.path.join(
            self.corpus_directory, self.short_name(), "LLVMFuzzer.TestOneInput", "seeds"
        )

        old_corpus_dir = f"{corpus_directory_stem}/corpus-{self.short_name()}"
        if os.path.exists(old_corpus_dir):
            os.makedirs(self.seed_directory, exist_ok=True)
            os.rename(old_corpus_dir, self.seed_directory)

        if not os.path.exists(self.seed_directory):
            self.seed_directory = ""

        os.makedirs(self.corpus_directory, exist_ok=True)

        interface.append_process_tracking_options(self.program_options, self._id)

    def _make_process(self):
        default_args = [
            self.program_executable,
            "--fuzz_for=1h",
            f"--corpus_database={self.corpus_directory}",
            f"--llvm_fuzzer_wrapper_corpus_dir={self.seed_directory}",
        ]
        # Merge fixture environment variables into program_options
        program_options = self.program_options.copy()
        self._merge_fixture_environment_variables(program_options)

        return core.programs.make_process(self.logger, default_args, **program_options)
