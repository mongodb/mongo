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
from datetime import datetime
from shlex import quote

import psutil
from buildscripts.resmokelib import config as _config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib import utils
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
        job_info = win32job.QueryInformationJobObject(job_object,
                                                      win32job.JobObjectExtendedLimitInformation)

        # Set up the job object so that closing the last handle to the job object
        # will terminate all associated processes and destroy the job object itself.
        job_info["BasicLimitInformation"]["LimitFlags"] |= \
                win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE

        # Update the limits of the job object.
        win32job.SetInformationJobObject(job_object, win32job.JobObjectExtendedLimitInformation,
                                         job_info)

        return job_object

    # Don't create a job object if the current process is already inside one.
    if win32job.IsProcessInJob(win32process.GetCurrentProcess(), None):
        _JOB_OBJECT = None
    else:
        _JOB_OBJECT = _init_job_object()
        atexit.register(win32api.CloseHandle, _JOB_OBJECT)


class Process(object):
    """Wrapper around subprocess.Popen class."""

    # pylint: disable=protected-access

    def __init__(self, logger, args, env=None, env_vars=None, cwd=None):
        """Initialize the process with the specified logger, arguments, and environment."""

        # Ensure that executable files that don't already have an
        # extension on Windows have a ".exe" extension.
        if sys.platform == "win32" and not os.path.splitext(args[0])[1]:
            args[0] += ".exe"

        self.logger = logger
        self.args = args

        self.env = utils.default_if_none(env, os.environ.copy())
        if not self.env.get('RESMOKE_PARENT_PROCESS'):
            self.env['RESMOKE_PARENT_PROCESS'] = os.environ.get('RESMOKE_PARENT_PROCESS',
                                                                str(os.getpid()))
        if not self.env.get('RESMOKE_PARENT_CTIME'):
            self.env['RESMOKE_PARENT_CTIME'] = os.environ.get('RESMOKE_PARENT_CTIME',
                                                              str(psutil.Process().create_time()))
        if env_vars is not None:
            self.env.update(env_vars)

        self.pid = None

        self._process = None
        self._recorder = None
        self._stdout_pipe = None
        self._stderr_pipe = None
        self._cwd = cwd

    def start(self):
        """Start the process and the logger pipes for its stdout and stderr."""

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
        close_fds = (sys.platform != "win32")

        with _POPEN_LOCK:

            # Record unittests directly since resmoke doesn't not interact with them and they can finish
            # too quickly for the recorder to have a chance at attaching.
            recorder_args = []
            if _config.UNDO_RECORDER_PATH is not None and self.args[0].endswith("_test"):
                now_str = datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
                # Only use the process name since we have to be able to correlate the recording name
                # with the binary name easily.
                recorder_output_file = "{process}-{t}.undo".format(
                    process=os.path.basename(self.args[0]), t=now_str)
                recorder_args = [_config.UNDO_RECORDER_PATH, "-o", recorder_output_file]

            self._process = subprocess.Popen(recorder_args + self.args, bufsize=buffer_size,
                                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                             close_fds=close_fds, env=self.env,
                                             creationflags=creation_flags, cwd=self._cwd)
            self.pid = self._process.pid

            if _config.UNDO_RECORDER_PATH is not None and (not self.args[0].endswith("_test")) and (
                    "mongod" in self.args[0] or "mongos" in self.args[0]):
                now_str = datetime.now().strftime("%Y-%m-%dT%H-%M-%S")
                recorder_output_file = "{logger}-{process}-{pid}-{t}.undo".format(
                    logger=self.logger.name.replace('/', '-'),
                    process=os.path.basename(self.args[0]), pid=self.pid, t=now_str)
                recorder_args = [
                    _config.UNDO_RECORDER_PATH, "-p",
                    str(self.pid), "-o", recorder_output_file
                ]
                self._recorder = subprocess.Popen(recorder_args, bufsize=buffer_size, env=self.env,
                                                  creationflags=creation_flags)

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
        if mode is None:
            mode = fixture_interface.TeardownMode.TERMINATE

        if sys.platform == "win32":

            # Attempt to cleanly shutdown mongod.
            if mode != fixture_interface.TeardownMode.KILL and self.args and self.args[0].find(
                    "mongod") != -1:
                mongo_signal_handle = None
                try:
                    mongo_signal_handle = win32event.OpenEvent(
                        win32event.EVENT_MODIFY_STATE, False,
                        "Global\\Mongo_" + str(self._process.pid))

                    if not mongo_signal_handle:
                        # The process has already died.
                        return
                    win32event.SetEvent(mongo_signal_handle)
                    # Wait 60 seconds for the program to exit.
                    status = win32event.WaitForSingleObject(self._process._handle, 60 * 1000)
                    if status == win32event.WAIT_OBJECT_0:
                        return
                except win32process.error as err:
                    # ERROR_FILE_NOT_FOUND (winerror=2)
                    # ERROR_ACCESS_DENIED (winerror=5)
                    # ERROR_INVALID_HANDLE (winerror=6)
                    # One of the above errors is received if the process has
                    # already died.
                    if err.winerror not in (2, 5, 6):
                        raise
                finally:
                    win32api.CloseHandle(mongo_signal_handle)

                print("Failed to cleanly exit the program, calling TerminateProcess() on PID: " +\
                    str(self._process.pid))

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
        else:
            try:
                if mode == fixture_interface.TeardownMode.KILL:
                    self._process.kill()
                elif mode == fixture_interface.TeardownMode.TERMINATE:
                    self._process.terminate()
                elif mode == fixture_interface.TeardownMode.ABORT:
                    self._process.send_signal(mode.value)
                else:
                    raise errors.ProcessError("Process wrapper given unrecognized teardown mode: " +
                                              mode.value)

            except OSError as err:
                # ESRCH (errno=3) is received when the process has already died.
                if err.errno != 3:
                    raise

    def poll(self):
        """Poll."""
        return self._process.poll()

    def wait(self, timeout=None):
        """Wait until process has terminated and all output has been consumed by the logger pipes."""

        return_code = self._process.wait(timeout)

        if self._recorder is not None:
            self.logger.info('Saving the UndoDB recording; it may take a few minutes...')
            recorder_return = self._recorder.wait(timeout)
            if recorder_return != 0:
                raise errors.ServerFailure(
                    "UndoDB live-record did not terminate correctly. This is likely a bug with UndoDB. "
                    "Please record the logs and notify the #server-testing Slack channel")

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
        self._process.send_signal(signal.SIGCONT)

    def __str__(self):
        if self.pid is None:
            return self.as_command()
        return "%s (%d)" % (self.as_command(), self.pid)
