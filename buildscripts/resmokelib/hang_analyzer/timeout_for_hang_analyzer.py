import threading

from buildscripts.resmokelib.flags import HANG_ANALYZER_CALLED


class TimeoutForHangAnalyzer:
    """Runs a function in a separate thread. If the hang analyzer has been called, raise a TimeoutError
    after the timeout duration. The function will continue to run in the background."""

    def __init__(self, timeout, func, args=()):
        self.func = func
        self.args = args
        self.result = None
        self.exception = None
        self.timeout = timeout

    def _worker(self):
        try:
            self.result = self.func(*self.args)
        except Exception as e:
            self.exception = e

    def run(self):
        thread = threading.Thread(target=self._worker)
        thread.start()

        while True:
            thread.join(self.timeout)
            if HANG_ANALYZER_CALLED.is_set() and thread.is_alive():
                raise TimeoutError(
                    f"Function {self.func} execution exceeded the time limit of {self.timeout} seconds."
                )
            elif not thread.is_alive():
                if self.exception:
                    raise self.exception
                return self.result
