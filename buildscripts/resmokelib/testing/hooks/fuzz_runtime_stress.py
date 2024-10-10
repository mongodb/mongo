import math
import multiprocessing
import os
import random
import threading
import time

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.testing.hooks import interface
from buildscripts.resmokelib.testing.hooks import lifecycle as lifecycle_interface


class FuzzRuntimeStress(interface.Hook):
    """Test hook that periodically changes the amount of stress the system is experiencing."""

    DESCRIPTION = "Changes the amount of system stress at regular intervals"

    IS_BACKGROUND = True

    def __init__(self, hook_logger, fixture, option="off"):
        """Initialize the FuzzRuntimeStress."""
        self._option = option
        interface.Hook.__init__(self, hook_logger, fixture, FuzzRuntimeStress.DESCRIPTION)
        self._stress_thread = None

    def before_suite(self, test_report):
        """Before suite."""
        self._stress_thread = _FuzzStressThread(
            self.logger,
            lifecycle_interface.FlagBasedThreadLifecycle(),
            self._option,
        )
        self.logger.info("Starting the runtime stress fuzzing thread.")
        self._stress_thread.start()

    def after_suite(self, test_report, teardown_flag=None):
        """After suite."""
        self.logger.info("Stopping the runtime stress fuzzing thread.")
        self._stress_thread.stop()
        self.logger.info("Runtime stress fuzzing thread stopped.")

    def before_test(self, test, test_report):
        """Before test."""
        self.logger.info("Resuming the runtime stress fuzzing thread.")
        self._stress_thread.pause()
        self._stress_thread.resume()

    def after_test(self, test, test_report):
        """After test."""
        self.logger.info("Pausing the runtime stress fuzzing thread.")
        self._stress_thread.pause()
        self.logger.info("Paused the runtime stress fuzzing thread.")


class _FuzzStressThread(threading.Thread):
    # Max number of processes the cpu stress should create per core
    MAX_PROCESSES_PER_CORE = 1
    INTERVAL_BETWEEN_FUZZ = 20

    def __init__(
        self,
        logger,
        lifecycle,
        option,
    ):
        """Initialize _FuzzStressThread."""
        threading.Thread.__init__(self, name="FuzzStressThread")
        self.daemon = True
        self.logger = logger
        self.__lifecycle = lifecycle
        self._option = option

        self._interval_secs = _FuzzStressThread.INTERVAL_BETWEEN_FUZZ
        self._last_exec = 0

        self._processes = []

    def _spin():
        """Loop for increasing CPU utilization."""
        while True:
            c = 12345
            # Intent is to stress both ALU and FPU
            math.sqrt(c)
            _ = c * c

    def fuzz_cpu_stress(self):
        """Stress CPU with a random amount of processes."""
        num_cores = os.cpu_count()
        # Spawn a maximum of (num_cores * MAX_PROCESSES_PER_CORE) processes or a minimum of 0 processes.
        num_processes = random.randrange(1, num_cores + 1) * random.randrange(
            _FuzzStressThread.MAX_PROCESSES_PER_CORE + 1
        )
        self.logger.info(f"Stress fuzzing thread is spawning {num_processes} processes")
        for _ in range(num_processes):
            proc = multiprocessing.Process(target=_FuzzStressThread._spin, daemon=True)
            self._processes.append(proc)
            proc.start()

    def stress(self):
        """Create processes to simulate stress depending on the option."""
        self.logger.exception(f"Stress fuzzing thread creating {self._option} stress")
        if self._option == "cpu":
            self.fuzz_cpu_stress()

    def clear_stress(self):
        """Clear all processes spawned by this thread."""
        self.logger.exception(f"Stress fuzzing thread clearing {len(self._processes)} processes")
        for proc in self._processes:
            proc.terminate()
        self._processes.clear()

    def run(self):
        """Execute the thread."""
        try:
            while True:
                permitted = self.__lifecycle.wait_for_action_permitted()
                if not permitted:
                    break

                # Make sure ongoing stress processes are killed.
                self.clear_stress()

                now = time.time()
                if now - self._last_exec > self._interval_secs:
                    self.stress()
                    self._last_exec = time.time()

                # The 'wait_secs' is used to wait 'self._interval_secs' from the moment
                # the last `stress` was called.
                now = time.time()
                wait_secs = max(0, self._interval_secs - (now - self._last_exec))
                self.__lifecycle.wait_for_action_interval(wait_secs)
        except Exception:  # pylint: disable=W0703
            # Proactively log the exception when it happens so it will be
            # flushed immediately.
            self.logger.exception("Stress fuzzing thread threw exception")
            self.clear_stress()

    def stop(self):
        """Stop the thread."""
        self.__lifecycle.stop()
        # Unpause to allow the thread to finish.
        self.resume()
        self.join()

    def pause(self):
        """Pause the thread."""
        self.__lifecycle.mark_test_finished()

        # Check if the thread is alive in case it has thrown an exception while running.
        self._check_thread()

    def resume(self):
        """Resume the thread."""
        self.__lifecycle.mark_test_started()

    def _check_thread(self):
        if not self.is_alive():
            msg = "Stress fuzzing thread is not running."
            self.logger.error(msg)
            raise errors.ServerFailure(msg)
