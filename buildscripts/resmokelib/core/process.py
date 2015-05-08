"""
A more reliable way to create and destroy processes.

Uses job objects when running on Windows to ensure that all created
processes are terminated.
"""

from __future__ import absolute_import

import logging
import os
import os.path
import subprocess
import sys
import threading

from . import pipe
from .. import utils

# Prevent race conditions when starting multiple subprocesses on the same thread.
# See https://bugs.python.org/issue2320 for more details.
_POPEN_LOCK = threading.Lock()

# Job objects are the only reliable way to ensure that processes are terminated on Windows.
if sys.platform == "win32":
    import win32con
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

        # TODO: register an atexit handler to ensure that the job object handle gets closed
        return job_object

    _JOB_OBJECT = _init_job_object()


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
        if sys.platform == "win32":
            creation_flags |= win32process.CREATE_BREAKAWAY_FROM_JOB

        with _POPEN_LOCK:
            self._process = subprocess.Popen(self.args,
                                             env=self.env,
                                             creationflags=creation_flags,
                                             stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE)
            self.pid = self._process.pid

        self._stdout_pipe = pipe.LoggerPipe(self.logger, logging.INFO, self._process.stdout)
        self._stderr_pipe = pipe.LoggerPipe(self.logger, logging.ERROR, self._process.stderr)

        self._stdout_pipe.wait_until_started()
        self._stderr_pipe.wait_until_started()

        if sys.platform == "win32":
            try:
                win32job.AssignProcessToJobObject(_JOB_OBJECT, self._process._handle)
            except win32job.error as err:
                # ERROR_ACCESS_DENIED (winerror=5) is received when the process has already died.
                if err.winerror != winerror.ERROR_ACCESS_DENIED:
                    raise
                return_code = win32process.GetExitCodeProcess(self._process._handle)
                if return_code == win32con.STILL_ACTIVE:
                    raise

    def stop(self):
        """
        Terminates the process.
        """

        if sys.platform == "win32":
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

        sb = []
        for env_var in env_diff:
            sb.append("%s=%s" % (env_var, env_diff[env_var]))
        sb.extend(self.args)

        return " ".join(sb)

    def __str__(self):
        if self.pid is None:
            return self.as_command()
        return "%s (%d)" % (self.as_command(), self.pid)
