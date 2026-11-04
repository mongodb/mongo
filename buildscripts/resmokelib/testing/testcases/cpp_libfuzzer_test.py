"""The libfuzzertest.TestCase for C++ libfuzzer tests."""

import datetime
import os
from logging import Handler, LogRecord
from typing import Optional

from buildscripts.resmokelib import core, logging, utils
from buildscripts.resmokelib.testing.testcases import interface

# Centipede logs one of these (see centipede/runner_result.h) whenever a batch times out. It only
# saves a crash file if the slow input times out again when rerun on its own, so for every other
# timeout the log is the only record.
CENTIPEDE_TIMEOUT_FAILURES = ("per-input-timeout-exceeded", "per-batch-timeout-exceeded")


class _TimeoutDetector(Handler):
    """Watch the fuzzer's output and remember the first timeout it reports."""

    def __init__(self) -> None:
        super().__init__()
        self.timeout: str | None = None

    def emit(self, record: LogRecord) -> None:
        if self.timeout is None:
            message = record.getMessage()
            self.timeout = next((f for f in CENTIPEDE_TIMEOUT_FAILURES if f in message), None)


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
        """Run the fuzzer, then fail if it reported a timeout or left crash files in the corpus."""
        timeouts = _TimeoutDetector()
        self.logger.addHandler(timeouts)
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
        finally:
            self.logger.removeHandler(timeouts)

        problems = []
        # Centipede stores findings under <corpus>/<binary>/<test>/crashing/.
        # Check every named FuzzTest as well as LLVMFuzzer.TestOneInput. Retained
        # findings do not necessarily reproduce during an interrupted run or
        # originate in the current build.
        crash_files = self._get_crash_files()
        if crash_files:
            problems.append(
                f"has {len(crash_files)} retained crash(es) "
                f"in the corpus database: {', '.join(crash_files)}"
            )
        if timeouts.timeout:
            problems.append(f"reported {timeouts.timeout} (see docs/fuzztest.md#timeouts)")

        if problems:
            self.return_code = 1
            msg = f"{self.short_description()} {'; '.join(problems)}"
            self.logger.error(msg)
            raise self.failureException(msg)

    def _get_crash_files(self) -> list[str]:
        """Find retained inputs for this binary, without suppressing corpus access errors."""
        binary_corpus = os.path.join(self.corpus_directory, self.short_name())
        try:
            tests = os.scandir(binary_corpus)
        except FileNotFoundError:
            return []

        crash_files: list[str] = []
        with tests:
            for test in tests:
                if not test.is_dir():
                    continue

                try:
                    crashes = os.scandir(os.path.join(test.path, "crashing"))
                except FileNotFoundError:
                    continue

                with crashes:
                    crash_files.extend(crash.path for crash in crashes if crash.is_file())
        return sorted(crash_files)

    def _make_process(self):
        default_args = [
            self.program_executable,
            "--fuzz_for=1h",
            # Far longer than any input takes in the nightly runs, so only a hang or a pathologically
            # slow input reaches it. See docs/fuzztest.md#timeouts.
            "--time_limit_per_input=600s",
            f"--corpus_database={self.corpus_directory}",
            f"--llvm_fuzzer_wrapper_corpus_dir={self.seed_directory}",
        ]
        # Merge test and fixture environment variables into program_options
        program_options = self.program_options.copy()
        self._merge_environment_variables(program_options)

        return core.programs.make_process(self.logger, default_args, **program_options)
