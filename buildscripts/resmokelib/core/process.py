"""
A more reliable way to create and destroy processes.

Uses job objects when running on Windows to ensure that all created
processes are terminated.
"""

from __future__ import absolute_import

import atexit
import logging
import os
import os.path
import sys
import threading

# The subprocess32 module resolves the thread-safety issues of the subprocess module in Python 2.x
# when the _posixsubprocess C extension module is also available. Additionally, the _posixsubprocess
# C extension module avoids triggering invalid free() calls on Python's internal data structure for
# thread-local storage by skipping the PyOS_AfterFork() call when the 'preexec_fn' parameter isn't
# specified to subprocess.Popen(). See SERVER-22219 for more details.
#
# The subprocess32 module is untested on Windows and thus isn't recommended for use, even when it's
# installed. See https://github.com/google/python-subprocess32/blob/3.2.7/README.md#usage.
if os.name == "posix" and sys.version_info[0] == 2:
    try:
        import subprocess32 as subprocess
    except ImportError:
        import warnings
        warnings.warn(("Falling back to using the subprocess module because subprocess32 isn't"
                       " available. When using the subprocess module, a child process may trigger"
                       " an invalid free(). See SERVER-22219 for more details."),
                      RuntimeWarning)
        import subprocess
else:
    import subprocess

from . import pipe
from .. import utils

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
        win32job.SetInformationJobObject(job_object,
                                         win32job.JobObjectExtendedLimitInformation,
                                         job_info)

        return job_object

    # Don't create a job object if the current process is already inside one.
    if win32job.IsProcessInJob(win32process.GetCurrentProcess(), None):
        _JOB_OBJECT = None
    else:
        _JOB_OBJECT = _init_job_object()
        atexit.register(win32api.CloseHandle, _JOB_OBJECT)


class Process(object):
    """
    Wrapper around subprocess.Popen class.
    """

    def __init__(self, logger, args, env=None, env_vars=None):
        """
        Initializes the process with the specified logger, arguments,
        and environment.
        """

        # Ensure that executable files on Windows have a ".exe" extension.
        if sys.platform == "win32" and os.path.splitext(args[0])[1] != ".exe":
            args[0] += ".exe"

        self.logger = logger
        self.args = args
        self.env = utils.default_if_none(env, os.environ.copy())
        if env_vars is not None:
            self.env.update(env_vars)

        self.pid = None

        self._process = None
        self._stdout_pipe = None
        self._stderr_pipe = None

    def start(self):
        """
        Starts the process and the logger pipes for its stdout and
        stderr.
        """

        creation_flags = 0
        if sys.platform == "win32" and _JOB_OBJECT is not None:
            creation_flags |= win32process.CREATE_BREAKAWAY_FROM_JOB

        # Use unbuffered I/O pipes to avoid adding delay between when the subprocess writes output
        # and when the LoggerPipe thread reads it.
        buffer_size = 0

        # Close file descriptors in the child process before executing the program. This prevents
        # file descriptors that were inherited due to multiple calls to fork() -- either within one
        # thread, or concurrently from multiple threads -- from causing another subprocess to wait
        # for the completion of the newly spawned child process. Closing other file descriptors
        # isn't supported on Windows when stdout and stderr are redirected.
        close_fds = (sys.platform != "win32")

        with _POPEN_LOCK:
            self._process = subprocess.Popen(self.args,
                                             bufsize=buffer_size,
                                             stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE,
                                             close_fds=close_fds,
                                             env=self.env,
                                             creationflags=creation_flags)
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

    def stop(self, kill=False):
        """Terminate the process."""
        if sys.platform == "win32":

            # Attempt to cleanly shutdown mongod.
            if not kill and len(self.args) > 0 and self.args[0].find("mongod") != -1:
                mongo_signal_handle = None
                try:
                    mongo_signal_handle = win32event.OpenEvent(
                        win32event.EVENT_MODIFY_STATE, False, "Global\\Mongo_" +
                        str(self._process.pid))

                    if not mongo_signal_handle:
                        # The process has already died.
                        return
                    win32event.SetEvent(mongo_signal_handle)
                    # Wait 60 seconds for the program to exit.
                    status = win32event.WaitForSingleObject(
                        self._process._handle, 60 * 1000)
                    if status == win32event.WAIT_OBJECT_0:
                        return
                except win32process.error as err:
                    # ERROR_FILE_NOT_FOUND (winerror=2)
                    # ERROR_ACCESS_DENIED (winerror=5)
                    # ERROR_INVALID_HANDLE (winerror=6)
                    # One of the above errors is received if the process has
                    # already died.
                    if err[0] not in (2, 5, 6):
                        raise
                finally:
                    win32api.CloseHandle(mongo_signal_handle)

                print "Failed to cleanly exit the program, calling TerminateProcess() on PID: " +\
                    str(self._process.pid)

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
                if kill:
                    self._process.kill()
                else:
                    self._process.terminate()
            except OSError as err:
                # ESRCH (errno=3) is received when the process has already died.
                if err.errno != 3:
                    raise

    def poll(self):
        return self._process.poll()

    def wait(self):
        """
        Waits until the process has terminated and all output has been
        consumed by the logger pipes.
        """

        return_code = self._process.wait()

        if self._stdout_pipe:
            self._stdout_pipe.wait_until_finished()
        if self._stderr_pipe:
            self._stderr_pipe.wait_until_finished()

        return return_code

    def as_command(self):
        """
        Returns an equivalent command line invocation of the process.
        """

        default_env = os.environ
        env_diff = self.env.copy()

        # Remove environment variables that appear in both 'os.environ' and 'self.env'.
        for env_var in default_env:
            if env_var in env_diff and env_diff[env_var] == default_env[env_var]:
                del env_diff[env_var]

        sb = []  # String builder.
        for env_var in env_diff:
            sb.append("%s=%s" % (env_var, env_diff[env_var]))
        sb.extend(self.args)

        return " ".join(sb)

    def __str__(self):
        if self.pid is None:
            return self.as_command()
        return "%s (%d)" % (self.as_command(), self.pid)
