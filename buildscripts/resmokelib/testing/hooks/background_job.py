"""Background job utils. Helps run a JS file repeatedly while a test suite's tests run."""

import sys
import threading

from buildscripts.resmokelib.testing.hooks import jsfile


class _BackgroundJob(threading.Thread):
    """A thread for running a JS file while a test is running."""

    def __init__(self, thread_name):
        """Initialize _BackgroundJob."""
        threading.Thread.__init__(self, name=thread_name)
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
        """
        Signal the background thread that it should stop executing 'self._hook_test_case'.

        Wait until the current execution has finished.
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
