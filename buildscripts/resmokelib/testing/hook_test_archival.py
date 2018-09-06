"""
Enables supports for archiving tests or hooks.
"""

from __future__ import absolute_import

import os
import threading

from .. import config
from .. import utils
from ..utils import globstar


class HookTestArchival(object):
    """
    Archives hooks and tests to S3.
    """

    def __init__(self, suite, hooks, archive_instance, archive_config):
        self.archive_instance = archive_instance
        archive_config = utils.default_if_none(archive_config, {})

        self.on_success = archive_config.get("on_success", False)

        self.tests = []
        if "tests" in archive_config:
            # 'tests' is either a list of tests to archive or a bool (archive all if True).
            if not isinstance(archive_config["tests"], bool):
                for test in archive_config["tests"]:
                    self.tests += globstar.glob(test)
            elif archive_config["tests"]:
                self.tests = suite.tests

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

    def _should_archive(self, success):
        """ Return True if failed test or 'on_success' is True. """
        return not success or self.on_success

    def _archive_hook(self, logger, hook, test, success):
        """ Helper to archive hooks. """
        hook_match = hook.__class__.__name__ in self.hooks
        if not hook_match or not self._should_archive(success):
            return

        test_name = "{}:{}".format(test.short_name(), hook.__class__.__name__)
        self._archive_hook_or_test(logger, test_name, test)

    def _archive_test(self, logger, test, success):
        """ Helper to archive tests. """
        test_name = test.test_name
        test_match = False
        for arch_test in self.tests:
            # Ensure that the test_name is in the same format as the arch_test.
            if os.path.normpath(test_name) == os.path.normpath(arch_test):
                test_match = True
                break
        if not test_match or not self._should_archive(success):
            return

        self._archive_hook_or_test(logger, test_name, test)

    def archive(self, logger, test, success, hook=None):
        """ Archives data files for hooks or tests. """
        if not config.ARCHIVE_FILE or not self.archive_instance:
            return
        if hook:
            self._archive_hook(logger, hook, test, success)
        else:
            self._archive_test(logger, test, success)

    def _archive_hook_or_test(self, logger, test_name, test):
        """ Trigger archive of data files for a test or hook. """

        with self._lock:
            # Test repeat number is how many times the particular test has been archived.
            if test_name not in self._tests_repeat:
                self._tests_repeat[test_name] = 0
            else:
                self._tests_repeat[test_name] += 1
        # Normalize test path from a test or hook name.
        test_path = \
            test_name.replace("/", "_").replace("\\", "_").replace(".", "_").replace(":", "_")
        file_name = "mongo-data-{}-{}-{}-{}.tgz".format(
            config.EVERGREEN_TASK_ID,
            test_path,
            config.EVERGREEN_EXECUTION,
            self._tests_repeat[test_name])
        # Retrieve root directory for all dbPaths from fixture.
        input_files = test.fixture.get_dbpath_prefix()
        s3_bucket = config.ARCHIVE_BUCKET
        s3_path = "{}/{}/{}/datafiles/{}".format(
            config.EVERGREEN_PROJECT_NAME,
            config.EVERGREEN_VARIANT_NAME,
            config.EVERGREEN_REVISION,
            file_name)
        display_name = "Data files {} - Execution {} Repetition {}".format(
            test_name,
            config.EVERGREEN_EXECUTION,
            self._tests_repeat[test_name])
        logger.info("Archiving data files for test %s from %s", test_name, input_files)
        status, message = self.archive_instance.archive_files_to_s3(
            display_name, input_files, s3_bucket, s3_path)
        if status:
            logger.warning("Archive failed for %s: %s", test_name, message)
        else:
            logger.info("Archive succeeded for %s: %s", test_name, message)
