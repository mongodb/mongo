"""Wrapper for the ProcessControl class."""
import logging
import psutil

LOGGER = logging.getLogger(__name__)


class ProcessControl(object):
    """Process control class.

    Control processes either by name or a list of pids. If name is supplied, then
    all matching pids are controlled.
    """

    def __init__(self, name=None, pids=None):
        """Provide either 'name' or 'pids' to control the process."""
        if not name and not pids:
            raise Exception("Either 'process_name' or 'pids' must be specifed")
        self.name = name
        self.pids = []
        if pids:
            self.pids = pids
        self.procs = []

    def get_pids(self):
        """Return list of process ids for process 'self.name'."""
        if not self.name:
            return self.pids
        self.pids = []
        for proc in psutil.process_iter():
            try:
                if proc.name() == self.name:
                    self.pids.append(proc.pid)
            except psutil.NoSuchProcess:
                pass
        return self.pids

    def get_name(self):
        """Return process name or name of first running process from pids."""
        if not self.name:
            for pid in self.get_pids():
                proc = psutil.Process(pid)
                if psutil.pid_exists(pid):
                    self.name = proc.name()
                    break
        return self.name

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
        """Return true if any process is running that either matches on name or pids."""
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
                    LOGGER.info("Could not kill process with pid %d, as it no longer exists",
                                proc.pid)
