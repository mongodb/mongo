import threading
import time
from datetime import timedelta

from buildscripts.resmokelib.testing.hooks import interface


class SleepingHook(interface.Hook):
    IS_BACKGROUND = True
    REGISTERED_NAME = "SleepingHook"

    def __init__(self, hook_logger, fixture, sleep_time=timedelta(seconds=120)):
        self._sleep_time = sleep_time
        self._thread = None

    def before_test(self, test, test_report):
        self._thread = threading.Thread(target=time.sleep, args=(self._sleep_time.total_seconds(),))
        self._thread.start()

    def after_test(self, test, test_report):
        self._thread.join()
