"""Test hook that does maintainence tasks for libfuzzer tests."""

import os

from buildscripts.resmokelib import core
from buildscripts.resmokelib.testing.hooks import interface


class LibfuzzerHook(interface.Hook):
    """Merges inputs after a fuzzer run."""

    DESCRIPTION = "Merges inputs after a fuzzer run"

    IS_BACKGROUND = False

    def __init__(self, hook_logger, fixture):
        """Initialize the ContinuousStepdown.

        Args:
            hook_logger: the logger instance for this hook.
            fixture: the target fixture.
        """
        interface.Hook.__init__(self, hook_logger, fixture, LibfuzzerHook.DESCRIPTION)

        self._fixture = fixture

    def before_suite(self, test_report):
        """Before suite."""
        pass

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        pass

    def before_test(self, test, test_report):
        """Before test."""
        pass

    def after_test(self, test, test_report):
        """After test."""
        self._merge_corpus(test)

    def _merge_corpus(self, test):
        self.logger.info(f"Merge for {test.short_name()} libfuzzer test started, "
                         f"merging to {test.merged_corpus_directory}.")
        os.makedirs(test.merged_corpus_directory, exist_ok=True)
        default_args = [
            test.program_executable,
            "-merge=1",
            test.merged_corpus_directory,
            test.corpus_directory,
        ]
        process = core.programs.make_process(self.logger, default_args, **test.program_options)
        process.start()
        process.wait()
        self.logger.info(f"Merge for {test.short_name()} libfuzzer test finished.")
