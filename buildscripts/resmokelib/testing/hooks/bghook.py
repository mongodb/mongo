"""Base class for a hook that runs a thread in the background."""

import math
import threading

from buildscripts.resmokelib.testing.hooks import interface


class BGJob(threading.Thread):
    """
    Background job that continuously calls the 'run_action' function of the given hook.

    BGJob will call 'run_action' without any delay and expects the 'run_action' function to add some form of delay.
    """

    def __init__(self, hook, loop_delay_ms=None):
        """Initialize the background job."""
        threading.Thread.__init__(self, name=f"BGJob-{hook.__class__.__name__}")
        self._loop_delay_ms = loop_delay_ms
        self.daemon = True
        self._hook = hook
        self._interrupt_event = threading.Event()
        self.__is_alive = True
        self.err = None

    def run(self):
        """Run the background job."""
        while True:
            if self.__is_alive is False:
                break

            try:
                self._hook.run_action()
                if self._loop_delay_ms is not None:
                    # The configured loop delay asked us to wait before running the action again. Do
                    # that wait, but listen to see if we finish running the test or are killed in
                    # the meantime.
                    interrupted = self._interrupt_event.wait(self._loop_delay_ms / 1000.0)
                    if interrupted:
                        self._hook.logger.info("interrupted")
                        break
            except Exception as err:
                self._hook.logger.error("Background thread caught exception: %s.", err)
                self.err = err
                self.__is_alive = False

    def kill(self):
        """Kill the background job."""
        self.__is_alive = False
        self._interrupt_event.set()


class BGHook(interface.Hook):
    """A hook that repeatedly calls run_action() in a background thread for the duration of the test suite."""

    IS_BACKGROUND = True
    # By default, we continuously run the background hook for the duration of the suite.
    DEFAULT_TESTS_PER_CYCLE = math.inf

    def __init__(self, hook_logger, fixture, desc, tests_per_cycle=None, loop_delay_ms=None):
        """
        Initialize the background hook.

        'tests_per_cycle' or 'loop_delay_ms' can be used to configure how often the background job
        is restarted, and how often run_action() is called, respectively.
        """
        interface.Hook.__init__(self, hook_logger, fixture, desc)

        self.logger = hook_logger
        self.running_test = None
        self._background_job = None

        self._test_num = 0
        # The number of tests we execute before restarting the background hook.
        self._tests_per_cycle = (
            self.DEFAULT_TESTS_PER_CYCLE if tests_per_cycle is None else tests_per_cycle
        )
        self._loop_delay_ms = loop_delay_ms

    def run_action(self):
        """
        Perform an action. This function will be called continuously in the BgJob.

        If a sleep_delay_ms was given, that many milliseconds of sleep will happen between each
        invocation.
        """
        raise NotImplementedError

    def before_suite(self, test_report):
        """Start the background thread."""
        self.logger.info("Starting the background thread.")
        self._background_job = BGJob(self, self._loop_delay_ms)
        self._background_job.start()

    def after_suite(self, test_report, teardown_flag=None):
        """Signal the background thread to exit, and wait until it does."""

        self.logger.info("Stopping the background thread.")
        self._background_job.kill()
        self._background_job.join()

        if self._background_job.err is not None:
            self.logger.error("Encountered an error inside the hook: %s.", self._background_job.err)
            raise self._background_job.err

    def before_test(self, test, test_report):
        """Each test will call this before it executes."""
        self.running_test = test
        if self._background_job.is_alive():
            return

        self.logger.info("Restarting the background thread.")
        self._background_job = BGJob(self, self._loop_delay_ms)
        self._background_job.start()

    def after_test(self, test, test_report):
        """Each test will call this after it executes. Check if the hook found an error."""
        self._test_num += 1
        if self._test_num % self._tests_per_cycle != 0 and self._background_job.err is None:
            return

        self._background_job.kill()
        self._background_job.join()

        if self._background_job.err is not None:
            self.logger.error("Encountered an error inside the hook: %s.", self._background_job.err)
            raise self._background_job.err
        else:
            self.logger.info("Reached end of cycle in the hook, killing background thread.")
