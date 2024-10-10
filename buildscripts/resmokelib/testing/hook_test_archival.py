"""Enable support for archiving tests or hooks."""

import logging
import os
import threading
from typing import TYPE_CHECKING

from opentelemetry import trace

from buildscripts.resmokelib import config, errors, utils
from buildscripts.resmokelib.flags import HANG_ANALYZER_CALLED
from buildscripts.resmokelib.testing.suite import Suite
from buildscripts.resmokelib.testing.testcases.interface import TestCase
from buildscripts.resmokelib.utils import globstar

# TODO: if we ever fix the circular deps in resmoke we will be able to get rid of this
if TYPE_CHECKING:
    from buildscripts.resmokelib.testing.job import FixtureTestCaseManager, TestResult

TRACER = trace.get_tracer("resmoke")


class HookTestArchival(object):
    """Archive hooks and tests to S3."""

    def __init__(self, suite: Suite, hooks, archive_instance, archive_config):  # pylint: disable=unused-argument
        """Initialize HookTestArchival."""
        self.archive_instance = archive_instance
        archive_config = utils.default_if_none(archive_config, {})

        self.tests = []
        self.archive_all = False
        if "tests" in archive_config:
            # 'tests' is either a list of tests to archive or a bool (archive all if True).
            if not isinstance(archive_config["tests"], bool):
                for test in archive_config["tests"]:
                    self.tests += globstar.glob(test)
            elif archive_config["tests"]:
                self.archive_all = True

        self.hooks = []
        if "hooks" in archive_config:
            # 'hooks' is either a list of hooks to archive or a bool (archive all if True).
            if not isinstance(archive_config["hooks"], bool):
                self.hooks = archive_config["hooks"]
            elif archive_config["hooks"]:
                for hook in hooks:
                    self.hooks.append(hook["class"])
        self._tests_repeat = {}
        self._lock = threading.Lock()

    def archive(
        self,
        logger: logging.Logger,
        result: "TestResult",
        manager: "FixtureTestCaseManager",
    ):
        """
        Archive data files for hooks or tests.

        :param logger: Where the logging output should be placed.
        :param result: A TestResult named tuple containing the test, hook, and outcome.
        :param manager: FixtureTestCaseManager object for the calling Job.
        """

        success = result.success
        should_archive = (config.ARCHIVE_FILE and self.archive_instance) and not success

        if not should_archive:
            return

        if result.hook and result.hook.REGISTERED_NAME in self.hooks:
            test_name = "{}:{}".format(result.test.short_name(), result.hook.REGISTERED_NAME)
            should_archive = True
        else:
            test_name = result.test.test_name
            if self.archive_all:
                should_archive = True
            else:
                should_archive = False
                for arch_test in self.tests:
                    # Ensure that the test_name is in the same format as the arch_test.
                    if os.path.normpath(test_name) == os.path.normpath(arch_test):
                        should_archive = True
                        break

        if should_archive or config.FORCE_ARCHIVE_ALL_DATA_FILES:
            self._archive_hook_or_test(logger, test_name, result.test, manager)

    @TRACER.start_as_current_span("hook_test_archival._archive_hook_or_test")
    def _archive_hook_or_test(
        self,
        logger: logging.Logger,
        test_name: str,
        test: TestCase,
        manager: "FixtureTestCaseManager",
    ):
        """Trigger archive of data files for a test or hook."""

        archive_hook_or_test_span = trace.get_current_span()
        archive_hook_or_test_span.set_attributes(attributes=test.get_test_otel_attributes())

        # We can still attempt archiving even if the teardown fails.
        if not manager.teardown_fixture(logger, abort=True):
            logger.warning("Error while aborting test fixtures; data files may be invalid.")
        with self._lock:
            # Test repeat number is how many times the particular test has been archived.
            if test_name not in self._tests_repeat:
                self._tests_repeat[test_name] = 0
            else:
                self._tests_repeat[test_name] += 1
        # Normalize test path from a test or hook name.
        test_path = (
            test_name.replace("/", "_").replace("\\", "_").replace(".", "_").replace(":", "_")
        )
        file_name = "mongo-data-{}-{}-{}-{}.tgz".format(
            config.EVERGREEN_TASK_ID,
            test_path,
            config.EVERGREEN_EXECUTION,
            self._tests_repeat[test_name],
        )
        # Retrieve root directory for all dbPaths from fixture.
        input_files = test.fixture.get_path_for_archival()
        s3_bucket = config.ARCHIVE_BUCKET
        s3_path = "{}/{}/{}/datafiles/{}".format(
            config.EVERGREEN_PROJECT_NAME,
            config.EVERGREEN_VARIANT_NAME,
            config.EVERGREEN_REVISION,
            file_name,
        )
        display_name = "Data files {} - Execution {} Repetition {}".format(
            test_name, config.EVERGREEN_EXECUTION, self._tests_repeat[test_name]
        )
        logger.info("Archiving data files for test %s from %s", test_name, input_files)
        status, message = self.archive_instance.archive_files_to_s3(
            display_name, input_files, s3_bucket, s3_path
        )
        if status:
            logger.warning("Archive failed for %s: %s", test_name, message)
        else:
            logger.info("Archive succeeded for %s: %s", test_name, message)

        if HANG_ANALYZER_CALLED.is_set():
            logger.info("Hang Analyzer has been called. Fixtures will not be restarted.")
            raise errors.StopExecution(
                "Hang analyzer has been called. Stopping further execution of tests."
            )
        elif not manager.setup_fixture(logger):
            raise errors.StopExecution("Error while restarting test fixtures after archiving.")
