"""Wrapper for OS Service Wrappers."""
import importlib
import os
import sys
import time

from buildscripts.resmokelib.powercycle.lib.process_control import ProcessControl
from buildscripts.resmokelib.powercycle.lib import execute_cmd

_IS_WINDOWS = sys.platform == "win32" or sys.platform == "cygwin"


def _try_import(module, name=None):
    """Attempt to import a module and add it as a global variable.

    If the import fails, then this function doesn't trigger an exception.
    """
    try:
        module_name = module if not name else name
        globals()[module_name] = importlib.import_module(module)
    except ImportError:
        pass


if _IS_WINDOWS:
    # These modules are used on both sides for dumping python stacks.
    import win32api
    import win32event

    # These modules are used on the 'server' side.
    _try_import("ntsecuritycon")
    _try_import("pywintypes")
    _try_import("winerror")
    _try_import("win32file")
    _try_import("win32security")
    _try_import("win32service")
    _try_import("win32serviceutil")


# pylint: disable=undefined-variable,unused-variable
class WindowsService(object):
    """Windows service control class."""

    def __init__(self, name, bin_path, bin_options, db_path):
        """Initialize WindowsService."""

        self.name = name
        self.bin_name = os.path.basename(bin_path)
        self.bin_path = bin_path
        self.bin_options = bin_options
        self.db_path = db_path
        self.start_type = win32service.SERVICE_DEMAND_START
        self.pids = []
        self._states = {
            win32service.SERVICE_CONTINUE_PENDING: "continue pending",
            win32service.SERVICE_PAUSE_PENDING: "pause pending",
            win32service.SERVICE_PAUSED: "paused",
            win32service.SERVICE_RUNNING: "running",
            win32service.SERVICE_START_PENDING: "start pending",
            win32service.SERVICE_STOPPED: "stopped",
            win32service.SERVICE_STOP_PENDING: "stop pending",
        }

    def create(self):
        """Create service, if not installed. Return (code, output) tuple."""
        if self.status() in list(self._states.values()):
            return 1, "Service '{}' already installed, status: {}".format(self.name, self.status())
        try:
            win32serviceutil.InstallService(pythonClassString="Service.{}".format(
                self.name), serviceName=self.name, displayName=self.name, startType=self.start_type,
                                            exeName=self.bin_path, exeArgs=self.bin_options)
            ret = 0
            output = "Service '{}' created".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = f"{err.args[1]}: {err.args[2]}"

        return ret, output

    def update(self):
        """Update installed service. Return (code, output) tuple."""
        if self.status() not in self._states.values():
            return 1, "Service update '{}' status: {}".format(self.name, self.status())
        try:
            win32serviceutil.ChangeServiceConfig(pythonClassString="Service.{}".format(
                self.name), serviceName=self.name, displayName=self.name, startType=self.start_type,
                                                 exeName=self.bin_path, exeArgs=self.bin_options)
            ret = 0
            output = "Service '{}' updated".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = f"{err.args[1]}: {err.args[2]}"

        return ret, output

    def delete(self):
        """Delete service. Return (code, output) tuple."""
        if self.status() not in self._states.values():
            return 1, "Service delete '{}' status: {}".format(self.name, self.status())
        try:
            win32serviceutil.RemoveService(serviceName=self.name)
            ret = 0
            output = "Service '{}' deleted".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = f"{err.args[1]}: {err.args[2]}"

        return ret, output

    def start(self):
        """Start service. Return (code, output) tuple."""
        if self.status() not in self._states.values():
            return 1, "Service start '{}' status: {}".format(self.name, self.status())
        try:
            win32serviceutil.StartService(serviceName=self.name)
            ret = 0
            output = "Service '{}' started".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = f"{err.args[1]}: {err.args[2]}"

        proc = ProcessControl(name=self.bin_name)
        self.pids = proc.get_pids()

        return ret, output

    def stop(self, timeout):
        """Stop service, waiting for 'timeout' seconds. Return (code, output) tuple."""
        self.pids = []
        if self.status() not in self._states.values():
            return 1, "Service '{}' status: {}".format(self.name, self.status())
        try:
            win32serviceutil.StopService(serviceName=self.name)
            start = time.time()
            status = self.status()
            while status == "stop pending":
                if time.time() - start >= timeout:
                    ret = 1
                    output = "Service '{}' status is '{}'".format(self.name, status)
                    break
                time.sleep(3)
                status = self.status()
            ret = 0
            output = "Service '{}' stopped".format(self.name)
        except pywintypes.error as err:
            ret = err.winerror
            output = f"{err.args[1]}: {err.args[2]}"

            if ret == winerror.ERROR_BROKEN_PIPE:
                # win32serviceutil.StopService() returns a "The pipe has been ended" error message
                # (winerror=109) when stopping the "mongod-powercycle-test" service on
                # Windows Server 2016 and the underlying mongod process has already exited.
                ret = 0
                output = f"Assuming service '{self.name}' stopped despite error: {output}"

        return ret, output

    def status(self):
        """Return state of the service as a string."""
        try:
            # QueryServiceStatus returns a tuple:
            #   (scvType, svcState, svcControls, err, svcErr, svcCP, svcWH)
            # See https://msdn.microsoft.com/en-us/library/windows/desktop/ms685996(v=vs.85).aspx
            scv_type, svc_state, svc_controls, err, svc_err, svc_cp, svc_wh = (
                win32serviceutil.QueryServiceStatus(serviceName=self.name))
            if svc_state in self._states:
                return self._states[svc_state]
            return "unknown"
        except pywintypes.error:
            return "not installed"

    def get_pids(self):
        """Return list of pids for service."""
        return self.pids


# pylint: enable=undefined-variable,unused-variable
class PosixService(object):
    """Service control on POSIX systems.

    Simulates service control for background processes which fork themselves,
    i.e., mongod with '--fork'.
    """

    def __init__(self, name, bin_path, bin_options, db_path):
        """Initialize PosixService."""
        self.name = name
        self.bin_path = bin_path
        self.bin_name = os.path.basename(bin_path)
        self.bin_options = bin_options
        self.db_path = db_path
        self.pids = []

    def create(self):
        """Simulate create service. Returns (code, output) tuple."""
        return 0, None

    def update(self):
        """Simulate update service. Returns (code, output) tuple."""
        return 0, None

    def delete(self):
        """Simulate delete service. Returns (code, output) tuple."""
        return 0, None

    def start(self):
        """Start process. Returns (code, output) tuple."""
        cmd = "{} {}".format(self.bin_path, self.bin_options)
        ret, output = execute_cmd(cmd)
        if not ret:
            proc = ProcessControl(name=self.bin_name)
            self.pids = proc.get_pids()
        return ret, output

    def stop(self, timeout):  # pylint: disable=unused-argument
        """Crash the posix process process. Empty "pids" to signal to `status` the process was terminated. Returns (code, output) tuple."""
        proc = ProcessControl(name=self.bin_name)
        proc.kill()
        self.pids = []
        return 0, None

    def status(self):
        """Return status of service. If "pids" is empty due to a `stop` call, return that the process is stopped. Otherwise only return `stopped` when the lock file is removed."""
        if not self.get_pids():
            return "stopped"

        # Wait for the lock file to be deleted which concludes a clean shutdown.
        lock_file = os.path.join(self.db_path, "mongod.lock")
        if not os.path.exists(lock_file):
            self.pids = []
            return "stopped"

        try:
            if os.stat(lock_file).st_size == 0:
                self.pids = []
                return "stopped"
        except OSError:
            # The lock file was probably removed. Instead of being omnipotent with exception
            # interpretation, have a follow-up call observe the file does not exist.
            return "running"

        return "running"

    def get_pids(self):
        """Return list of pids for process."""
        return self.pids
