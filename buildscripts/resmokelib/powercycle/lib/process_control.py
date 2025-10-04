"""Wrapper for the ProcessControl class."""

import logging

import psutil

LOGGER = logging.getLogger(__name__)


class ProcessControl(object):
    """Process control class.

    Control processes by name. All matching by supplied name
    pids are controlled.
    """

    def __init__(self, name):
        """Provide 'name' to control the process."""
        self.name = name
        self.pids = []
        self.procs = []

    def get_pids(self):
        """Return list of process ids for process 'self.name'."""
        self.pids = []
        for proc in psutil.process_iter():
            try:
                if proc.name() == self.name:
                    self.pids.append(proc.pid)
            except psutil.NoSuchProcess:
                pass
        return self.pids

    def get_procs(self):
        """Return a list of 'proc' for the associated pids."""
        procs = []
        for pid in self.get_pids():
            try:
                procs.append(psutil.Process(pid))
            except psutil.NoSuchProcess:
                pass
        return procs

    def is_running(self):
        """Return true if any process is running that matches pids."""
        for pid in self.get_pids():
            if psutil.pid_exists(pid):
                return True
        return False

    def kill(self):
        """Kill all running processes that match the list of pids."""
        if self.is_running():
            for proc in self.get_procs():
                try:
                    proc.kill()
                except psutil.NoSuchProcess:
                    LOGGER.info(
                        "Could not kill process with pid %d, as it no longer exists", proc.pid
                    )
