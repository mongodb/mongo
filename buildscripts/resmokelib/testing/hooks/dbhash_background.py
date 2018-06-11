"""Test hook for verifying data consistency across a replica set.

Unlike dbhash.py, this version of the hook runs continously in a background thread while the test is
running.
"""

from __future__ import absolute_import

import os.path
import sys
import threading

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import jsfile
from buildscripts.resmokelib.testing.testcases import interface as testcase


class CheckReplDBHashInBackground(jsfile.JSHook):
    """A hook for comparing the dbhashes of all replica set members while a test is running."""

    def __init__(self, hook_logger, fixture, shell_options=None):
        """Initialize CheckReplDBHashInBackground."""
        description = "Check dbhashes of all replica set members while a test is running"
        js_filename = os.path.join("jstests", "hooks", "run_check_repl_dbhash_background.js")
        jsfile.JSHook.__init__(self, hook_logger, fixture, js_filename, description,
                               shell_options=shell_options)

        self._background_job = None

    def before_suite(self, test_report):
        """Start the background thread."""
        client = self.fixture.mongo_client()
        server_status = client.admin.command("serverStatus")
        if not server_status["storageEngine"].get("supportsSnapshotReadConcern", False):
            self.logger.info("Not enabling the background thread because '%s' storage engine"
                             " doesn't support snapshot reads.",
                             server_status["storageEngine"]["name"])
            return
        if not server_status["storageEngine"].get("persistent", False):
            self.logger.info("Not enabling the background thread because '%s' storage engine"
                             " is not persistent.", server_status["storageEngine"]["name"])
            return

        self._background_job = _BackgroundJob()
        self.logger.info("Starting the background thread.")
        self._background_job.start()

    def after_suite(self, test_report):
        """Signal the background thread to exit, and wait until it does."""
        if self._background_job is None:
            return

        self.logger.info("Stopping the background thread.")
        self._background_job.stop()

    def before_test(self, test, test_report):
        """Instruct the background thread to run the dbhash check while 'test' is also running."""
        if self._background_job is None:
            return

        hook_test_case = _ContinuousDynamicJSTestCase.create_before_test(
            self.logger.test_case_logger, test, self, self._js_filename, self._shell_options)
        hook_test_case.configure(self.fixture)

        self.logger.info("Resuming the background thread.")
        self._background_job.resume(hook_test_case, test_report)

    def after_test(self, test, test_report):  # noqa: D205,D400
        """Instruct the background thread to stop running the dbhash check now that 'test' has
        finished running.
        """
        if self._background_job is None:
            return

        self.logger.info("Pausing the background thread.")
        self._background_job.pause()

        if self._background_job.exc_info is not None:
            if isinstance(self._background_job.exc_info[1], errors.TestFailure):
                # If the mongo shell process running the JavaScript file exited with a non-zero
                # return code, then we raise an errors.ServerFailure exception to cause resmoke.py's
                # test execution to stop.
                raise errors.ServerFailure(self._background_job.exc_info[1].args[0])
            else:
                self.logger.error("Encountered an error inside the background thread.",
                                  exc_info=self._background_job.exc_info)
                raise self._background_job.exc_info[1]


class _BackgroundJob(threading.Thread):  # pylint: disable=too-many-instance-attributes
    """A thread for running the dbhash check while a test is running."""

    def __init__(self):
        """Initialize _BackgroundJob."""
        threading.Thread.__init__(self, name="CheckReplDBHashInBackground")
        self.daemon = True

        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)

        self._should_stop = False
        self._should_resume = False
        self._is_idle = True

        self._hook_test_case = None
        self._test_report = None
        self.exc_info = None

    def run(self):
        while True:
            with self._lock:
                while not self._should_resume:
                    self._cond.wait()

                # We have the background thread set 'self._should_resume' back to false to ensure
                # that 'self._hook_test_case.run_dynamic_test()' is only called once before the
                # resume() method is called again.
                self._should_resume = False

                if self._should_stop:
                    break

                # We are about to execute 'self._hook_test_case' so we mark the background thread as
                # no longer being idle.
                self._is_idle = False

            try:
                self._hook_test_case.run_dynamic_test(self._test_report)
            except:  # pylint: disable=bare-except
                self.exc_info = sys.exc_info()
            finally:
                with self._lock:
                    self._is_idle = True
                    self._cond.notify_all()

    def stop(self):
        """Signal the background thread to exit, and wait until it does."""
        with self._lock:
            self._should_stop = True
        self.resume(hook_test_case=None, test_report=None)
        self.join()

    def pause(self):  # noqa: D205,D400
        """Signal the background thread that it should stop executing 'self._hook_test_case', and
        wait until the current execution has finished.
        """
        self._hook_test_case.signal_stop_test()
        with self._lock:
            while not self._is_idle:
                self._cond.wait()

    def resume(self, hook_test_case, test_report):
        """Instruct the background thread to start executing 'hook_test_case'."""
        self._hook_test_case = hook_test_case
        self._test_report = test_report
        self.exc_info = None
        with self._lock:
            self._should_resume = True
            self._cond.notify_all()


class _ContinuousDynamicJSTestCase(jsfile.DynamicJSTestCase):
    """A dynamic TestCase that runs a JavaScript file repeatedly."""

    def __init__(self, *args, **kwargs):
        """Initialize _ContinuousDynamicJSTestCase."""
        jsfile.DynamicJSTestCase.__init__(self, *args, **kwargs)
        self._should_stop = threading.Event()

    def run_test(self):
        """Execute the test repeatedly."""
        while not self._should_stop.is_set():
            jsfile.DynamicJSTestCase.run_test(self)

    def signal_stop_test(self):  # noqa: D205,D400
        """Indicate to the thread executing the run_test() method that the current execution is the
        last one. This method returns without waiting for the current execution to finish.
        """
        self._should_stop.set()
