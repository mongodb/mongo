"""The libfuzzertest.TestCase for C++ libfuzzer tests."""

import datetime
import glob
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
        **kwargs,
    ):
        """Initialize the CPPLibfuzzerTestCase with the executable to run."""

        assert len(program_executables) == 1
        interface.ProcessTestCase.__init__(
            self, logger, "C++ libfuzzer test", program_executables[0], **kwargs
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

    def run_test(self):
        """Run the test, then fail if any crash files were recorded in the corpus."""
        try:
            self.proc = self._make_process()
            self._execute(self.proc)
        except self.failureException:
            raise
        except:
            self.logger.exception(
                "Encountered an error running %s %s", self.test_kind, self.basename()
            )
            raise

        # Centipede's corpus database stores known-reproducible crashes (deduped
        # by signature) under <corpus>/<binary>/<test>/crashing/. It self-prunes
        # entries that no longer reproduce on the next run, so a non-empty
        # directory after the fuzzer exits means there are still-reproducible
        # crashes that this build is responsible for.
        crash_files = glob.glob(
            os.path.join(
                self.corpus_directory,
                self.short_name(),
                "LLVMFuzzer.TestOneInput",
                "crashing",
                "*",
            )
        )
        crash_files = [f for f in crash_files if os.path.isfile(f)]
        if crash_files:
            self.return_code = 1
            msg = f"{self.short_description()} has {len(crash_files)} reproducible crash(es) in the corpus database: {crash_files}"
            self.logger.error(msg)
            raise self.failureException(msg)

    def _make_process(self):
        default_args = [
            self.program_executable,
            "--fuzz_for=1h",
            f"--corpus_database={self.corpus_directory}",
            f"--llvm_fuzzer_wrapper_corpus_dir={self.seed_directory}",
        ]
        # Merge test and fixture environment variables into program_options
        program_options = self.program_options.copy()
        self._merge_environment_variables(program_options)

        return core.programs.make_process(self.logger, default_args, **program_options)
