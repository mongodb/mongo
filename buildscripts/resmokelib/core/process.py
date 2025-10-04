"""A more reliable way to create and destroy processes.

Uses job objects when running on Windows to ensure that all created
processes are terminated.
"""

import atexit
import logging
import os
import os.path
import signal
import subprocess
import sys
import threading
import time
from shlex import quote

import psutil

from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import errors, utils
from buildscripts.resmokelib.core import pipe
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface

# Attempt to avoid race conditions (e.g. hangs caused by a file descriptor being left open) when
# starting subprocesses concurrently from multiple threads by guarding calls to subprocess.Popen()
# with a lock. See https://bugs.python.org/issue2320 and https://bugs.python.org/issue12739 as
# reports of such hangs.
#
# This lock probably isn't necessary when both the subprocess32 module and its _posixsubprocess C
# extension module are available because either
#   (a) the pipe2() syscall is available on the platform we're using, so pipes are atomically
#       created with the FD_CLOEXEC flag set on them, or
#   (b) the pipe2() syscall isn't available, but the GIL isn't released during the
#       _posixsubprocess.fork_exec() call or the _posixsubprocess.cloexec_pipe() call.
# See https://bugs.python.org/issue7213 for more details.
_POPEN_LOCK = threading.Lock()

# we track the pids that we abort to know which core dumps are high priority to analyze
core_dump_file_pid_lock = threading.Lock()
BORING_CORE_DUMP_PIDS_FILE = "boring_core_dumps.txt"

# Job objects are the only reliable way to ensure that processes are terminated on Windows.
if sys.platform == "win32":
    import win32api
    import win32con
    import win32event
    import win32job
    import win32process
    import winerror

    def _init_job_object():
        job_object = win32job.CreateJobObject(None, "")

        # Get the limit and job state information of the newly-created job object.
        job_info = win32job.QueryInformationJobObject(
            job_object, win32job.JobObjectExtendedLimitInformation
        )

        # Set up the job object so that closing the last handle to the job object
        # will terminate all associated processes and destroy the job object itself.
        job_info["BasicLimitInformation"]["LimitFlags"] |= (
            win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        )

        # Update the limits of the job object.
        win32job.SetInformationJobObject(
            job_object, win32job.JobObjectExtendedLimitInformation, job_info
        )

        return job_object

    # Don't create a job object if the current process is already inside one.
    if win32job.IsProcessInJob(win32process.GetCurrentProcess(), None):
        _JOB_OBJECT = None
    else:
        _JOB_OBJECT = _init_job_object()
        atexit.register(win32api.CloseHandle, _JOB_OBJECT)


class Process(object):
    """Wrapper around subprocess.Popen class."""

    def __init__(self, logger, args, env=None, env_vars=None, cwd=None, start_new_session=False):
        """Initialize the process with the specified logger, arguments, and environment."""

        # Ensure that executable files that don't already have an
        # extension on Windows have a ".exe" extension.
        if sys.platform == "win32" and not os.path.splitext(args[0])[1]:
            args[0] += ".exe"

        self.logger = logger
        self.args = args

        self.env = utils.default_if_none(env, os.environ.copy())
        if not self.env.get("RESMOKE_PARENT_PROCESS"):
            self.env["RESMOKE_PARENT_PROCESS"] = os.environ.get(
                "RESMOKE_PARENT_PROCESS", str(os.getpid())
            )
        if not self.env.get("RESMOKE_PARENT_CTIME"):
            self.env["RESMOKE_PARENT_CTIME"] = os.environ.get(
                "RESMOKE_PARENT_CTIME", str(psutil.Process().create_time())
            )
        if env_vars is not None:
            self.env.update(env_vars)

        # If we are running against an External System Under Test & this is a `mongo{d,s}` process, we make this process a NOOP.
        # `mongo{d,s}` processes are not running locally for an External System Under Test.
        self.NOOP = _config.NOOP_MONGO_D_S_PROCESSES and os.path.basename(self.args[0]) in [
            "mongod",
            "mongos",
        ]

        # The `pid` attribute is assigned after the local process is started. If this process is a NOOP, we assign it a dummy value.
        self.pid = 1 if self.NOOP else None

        self._process = None
        self._stdout_pipe = None
        self._stderr_pipe = None
        self._cwd = cwd
        self._start_new_session = start_new_session

        if sys.platform == "win32":
            self._windows_shutdown_event_set = False

    def start(self):
        """Start the process and the logger pipes for its stdout and stderr."""
        if self.NOOP:
            return None

        creation_flags = 0
        if sys.platform == "win32" and _JOB_OBJECT is not None:
            creation_flags |= win32process.CREATE_BREAKAWAY_FROM_JOB

        # Tests fail if a process takes too long to startup and listen to a socket. Use buffered
        # I/O pipes to give the process some leeway.
        buffer_size = 1024 * 1024

        # Close file descriptors in the child process before executing the program. This prevents
        # file descriptors that were inherited due to multiple calls to fork() -- either within one
        # thread, or concurrently from multiple threads -- from causing another subprocess to wait
        # for the completion of the newly spawned child process. Closing other file descriptors
        # isn't supported on Windows when stdout and stderr are redirected.
        close_fds = sys.platform != "win32"

        with _POPEN_LOCK:
            self._process = subprocess.Popen(
                self.args,
                bufsize=buffer_size,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                close_fds=close_fds,
                env=self.env,
                creationflags=creation_flags,
                cwd=self._cwd,
                start_new_session=self._start_new_session,
            )
            self.pid = self._process.pid

        self._stdout_pipe = pipe.LoggerPipe(self.logger, logging.INFO, self._process.stdout)
        self._stderr_pipe = pipe.LoggerPipe(self.logger, logging.ERROR, self._process.stderr)

        self._stdout_pipe.wait_until_started()
        self._stderr_pipe.wait_until_started()

        if sys.platform == "win32" and _JOB_OBJECT is not None:
            try:
                win32job.AssignProcessToJobObject(_JOB_OBJECT, self._process._handle)
            except win32job.error as err:
                # ERROR_ACCESS_DENIED (winerror=5) is received when the process has already died.
                if err.winerror != winerror.ERROR_ACCESS_DENIED:
                    raise
                return_code = win32process.GetExitCodeProcess(self._process._handle)
                if return_code == win32con.STILL_ACTIVE:
                    raise

    def stop(self, mode=None):
        """Terminate the process."""
        if self.NOOP:
            return None

        if mode is None:
            mode = fixture_interface.TeardownMode.TERMINATE

        if sys.platform == "win32":
            if (
                mode != fixture_interface.TeardownMode.KILL
                and self.args
                and self.args[0].find("mongod") != -1
            ):
                self._request_clean_shutdown_on_windows()
            else:
                self._terminate_on_windows()
        else:
            try:
                if mode == fixture_interface.TeardownMode.KILL:
                    self._process.kill()
                elif mode == fixture_interface.TeardownMode.TERMINATE:
                    self._process.terminate()
                elif mode == fixture_interface.TeardownMode.ABORT:
                    # the core dumps taken when we abort are low prioirty to get analyzed.
                    # The data files are more useful most of the time.
                    # We poll the process just to make sure we don't count processes that
                    # ended prematurely as boring.
                    if _config.EVERGREEN_TASK_ID and self._process.poll() is None:
                        with core_dump_file_pid_lock:
                            pid = str(self._process.pid)
                            with open(BORING_CORE_DUMP_PIDS_FILE, "a") as file:
                                file.write(f"{pid}\n")
                    self._process.send_signal(mode.value)
                else:
                    raise errors.ProcessError(
                        "Process wrapper given unrecognized teardown mode: " + mode.value
                    )

            except OSError as err:
                # ESRCH (errno=3) is received when the process has already died.
                if err.errno != 3:
                    raise

    def poll(self):
        """Poll."""
        if self.NOOP:
            return None
        return self._process.poll()

    def wait(self, timeout=None):
        """Wait until process has terminated and all output has been consumed by the logger pipes."""
        if self.NOOP:
            return None

        if sys.platform == "win32" and self._windows_shutdown_event_set:
            status = None
            try:
                # Wait 60 seconds for the program to exit.
                status = win32event.WaitForSingleObject(self._process._handle, 60 * 1000)
            except win32process.error as err:
                # ERROR_FILE_NOT_FOUND (winerror=2)
                # ERROR_ACCESS_DENIED (winerror=5)
                # ERROR_INVALID_HANDLE (winerror=6)
                # One of the above errors is received if the process has
                # already died.
                if err.winerror not in (2, 5, 6):
                    raise

            if status is not None and status != win32event.WAIT_OBJECT_0:
                self.logger.info(
                    f"Failed to cleanly exit the program, calling TerminateProcess() on PID:"
                    f" {str(self._process.pid)}"
                )
                self._terminate_on_windows()

        return_code = self._process.wait(timeout)

        if self._stdout_pipe:
            self._stdout_pipe.wait_until_finished()
        if self._stderr_pipe:
            self._stderr_pipe.wait_until_finished()

        return return_code

    def as_command(self):
        """Return an equivalent command line invocation of the process."""

        default_env = os.environ
        env_diff = self.env.copy()

        # Remove environment variables that appear in both 'os.environ' and 'self.env'.
        for env_var in default_env:
            if env_var in env_diff and env_diff[env_var] == default_env[env_var]:
                del env_diff[env_var]

        sb = []  # String builder.
        for env_var in env_diff:
            sb.append(quote("%s=%s" % (env_var, env_diff[env_var])))
        sb.extend(map(quote, self.args))

        return " ".join(sb)

    def pause(self):
        """Send the SIGSTOP signal to the process and wait for it to be stopped."""
        if self.NOOP:
            return None
        while True:
            self._process.send_signal(signal.SIGSTOP)
            mongod_process = psutil.Process(self.pid)
            process_status = mongod_process.status()
            if process_status == psutil.STATUS_STOPPED:
                break
            self.logger.info("Process status: {}".format(process_status))
            time.sleep(1)

    def resume(self):
        """Send the SIGCONT signal to the process."""
        if self.NOOP:
            return None
        self._process.send_signal(signal.SIGCONT)

    def __str__(self):
        if self.pid is None:
            return self.as_command()
        return "%s (%d)" % (self.as_command(), self.pid)

    if sys.platform == "win32":

        def _request_clean_shutdown_on_windows(self):
            """Request clean shutdown on Windows."""
            _windows_mongo_signal_handle = None
            try:
                _windows_mongo_signal_handle = win32event.OpenEvent(
                    win32event.EVENT_MODIFY_STATE, False, "Global\\Mongo_" + str(self._process.pid)
                )

                if not _windows_mongo_signal_handle:
                    # The process has already died.
                    return
                win32event.SetEvent(_windows_mongo_signal_handle)
                self._windows_shutdown_event_set = True

            except win32process.error as err:
                # ERROR_FILE_NOT_FOUND (winerror=2)
                # ERROR_ACCESS_DENIED (winerror=5)
                # ERROR_INVALID_HANDLE (winerror=6)
                # One of the above errors is received if the process has
                # already died.
                if err.winerror not in (2, 5, 6):
                    raise

            finally:
                win32api.CloseHandle(_windows_mongo_signal_handle)

        def _terminate_on_windows(self):
            """Terminate the process on Windows."""
            # Adapted from implementation of Popen.terminate() in subprocess.py of Python 2.7
            # because earlier versions do not catch exceptions.
            try:
                # Have the process exit with code 0 if it is terminated by us to simplify the
                # success-checking logic later on.
                win32process.TerminateProcess(self._process._handle, 0)
            except win32process.error as err:
                # ERROR_ACCESS_DENIED (winerror=5) is received when the process
                # has already died.
                if err.winerror != winerror.ERROR_ACCESS_DENIED:
                    raise
                return_code = win32process.GetExitCodeProcess(self._process._handle)
                if return_code == win32con.STILL_ACTIVE:
                    raise
