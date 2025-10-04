import os
import random
import signal
import sys

from buildscripts.resmokelib.testing.hooks.bghook import BGHook


class PeriodicStackTrace(BGHook):
    """Test hook that sends the stacktracing signal to mongo processes at randomized intervals."""

    DESCRIPTION = "Sends the stacktracing signal to mongo processes at randomized intervals."

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture, frequency=1 / 60):
        """Initialize the hook."""
        BGHook.__init__(self, hook_logger, fixture, self.DESCRIPTION, loop_delay_ms=1000)
        self._fixture = fixture
        self._frequency = frequency

    def _signal_probability_per_iteration(self):
        return self._frequency

    def run_action(self):
        if sys.platform == "win32":
            return

        if random.random() <= self._signal_probability_per_iteration():
            pids = self._fixture.pids()
            if len(pids) == 0:
                return
            pid = random.choice(pids)
            self.logger.info(f"Requesting stacktrace from process {pid}")
            os.kill(pid, signal.SIGUSR2)
