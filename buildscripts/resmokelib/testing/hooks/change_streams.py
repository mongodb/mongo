"""Test hook to run change streams in the background."""

import random
import threading
import time

from pymongo import MongoClient

from buildscripts.resmokelib import config
from buildscripts.resmokelib.testing.hooks import interface


class RunChangeStreamsInBackground(interface.Hook):
    """A hook to run change streams in the background."""

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture):
        """Initialize RunChangeStreamsInBackground."""
        description = (
            "Run in the background full cluster change streams while a test is running."
            " Open and close the change stream every 1..10 tests (random using config.RANDOM_SEED)."
        )
        interface.Hook.__init__(self, hook_logger, fixture, description)
        self._fixture = fixture
        self._change_streams_thread = None
        self._test_run = None
        self._every_n_tests = random.randint(1, 10)
        self._full_suite_changes_num = 0

    def before_suite(self, test_report):
        """Print the log message."""
        self.logger.info(
            "Opening and closing change streams every %d tests. The seed is %d.",
            self._every_n_tests,
            config.RANDOM_SEED,
        )

    def after_suite(self, test_report, teardown_flag=None):
        """Stop the background thread."""
        if self._change_streams_thread is not None:
            self._stop_background_thread()

    def before_test(self, test, test_report):
        """Start the background thread if it is not already started."""
        if self._change_streams_thread is None:
            mongo_client = self._fixture.mongo_client()
            self._change_streams_thread = _ChangeStreamsThread(self.logger, mongo_client)
            self.logger.info("Starting the background change streams thread.")
            self._change_streams_thread.start()
            self._test_run = 0

    def after_test(self, test, test_report):
        """Every N tests stop the background thread."""
        self._test_run += 1
        if self._test_run == self._every_n_tests:
            self._stop_background_thread()

    def _stop_background_thread(self):
        """Signal the background thread to exit, and wait until it does."""
        self.logger.info("Stopping the background change streams thread.")
        self._change_streams_thread.stop()
        self._full_suite_changes_num += self._change_streams_thread.get_changes_number()
        self._change_streams_thread = None


class _ChangeStreamsThread(threading.Thread):
    """Change streams thread."""

    def __init__(self, logger, mongo_client: MongoClient) -> None:
        super().__init__(name="ChangeStreamsThread")
        self.daemon = True
        self.logger = logger
        self._mongo_client = mongo_client
        self._stop_iterating = threading.Event()
        self._changes_num = 0

    def run(self) -> None:
        """Execute the thread."""
        with self._mongo_client.watch() as stream:
            self.logger.info("Opening the change stream in the background.")
            while stream.alive and not self._stop_iterating.is_set():
                try:
                    change = stream.try_next()
                except Exception as err:
                    self.logger.error(
                        "Failed to get the next change from the change stream: %s", err
                    )
                else:
                    if change is None:
                        # Since there are tests that are running under 1s, we are sleeping here for just 10ms
                        time.sleep(0.01)
                    else:
                        self.logger.info("Change document: %r", change)
                        self._changes_num += 1

    def stop(self) -> None:
        """Stop the thread."""
        self._stop_iterating.set()
        self.join()

    def get_changes_number(self) -> int:
        """Return the number of changes."""
        return self._changes_num
