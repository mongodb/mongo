
"""
Module for simple execution of external programs with keyword arguments.

Also supports piping output into standard logging utilities.
"""


import logging
import os
import threading
import sys
import subprocess

KWARG_TYPE_IGNORE = -1
KWARG_TYPE_NORMAL = 0
KWARG_TYPE_EQUAL = 1
KWARG_TYPE_MULTIPLE = 2
KWARG_TYPE_CALLBACK = 3


def apply_json_args(process, json_doc, custom_kwargs={}):
    """Translate keyword arguments (JSON) into an argument list for an external process.

    CALLBACK-type args can do arbitrary things to the process being started (set env vars, change
    the process name, etc.).

    """

    for field in json_doc:

        kwarg, kwarg_type = ("--" + field, KWARG_TYPE_NORMAL) if field not in custom_kwargs \
            else custom_kwargs[field][0:2]
        value = json_doc[field]

        if kwarg_type == KWARG_TYPE_NORMAL:

            if value is not None:
                process.arguments.append(kwarg)
                if str(value):
                    process.arguments.append(str(value))

        elif kwarg_type == KWARG_TYPE_EQUAL:

            process.arguments.append(kwarg + "=" + str(value))

        elif kwarg_type == KWARG_TYPE_MULTIPLE:

            for ind_value in value:
                process.arguments.append(kwarg)
                process.arguments.append(str(ind_value))

        elif kwarg_type == KWARG_TYPE_CALLBACK:

            cl_arg_callback = custom_kwargs[field][2]
            cl_arg_callback(process, field, value)


class LoggerPipe(threading.Thread):

    """Monitors an external program's output and sends it to a logger."""

    def __init__(self, logger, level, pipe_out):
        threading.Thread.__init__(self)

        self.logger = logger
        self.level = level
        self.pipe_out = pipe_out

        self.lock = threading.Lock()
        self.condition = threading.Condition(self.lock)

        self.started = False
        self.finished = False

        self.start()

    def run(self):
        with self.lock:
            self.started = True
            self.condition.notify_all()

        for line in self.pipe_out:
            self.logger.log(self.level, line.strip())

        with self.lock:
            self.finished = True
            self.condition.notify_all()

    def wait_until_started(self):
        with self.lock:
            while not self.started:
                self.condition.wait()

    def wait_until_finished(self):
        with self.lock:
            while not self.finished:
                self.condition.wait()

    def flush(self):
        for handler in self.logger.handlers:
            handler.flush()


class ExternalContext(object):

    def __init__(self, env=None, env_vars={}, logger=None, **kwargs):
        self.env = env
        self.env_vars = env_vars
        self.logger = logger
        if not logger:
            return logging.getLogger("")
        self.kwargs = dict(kwargs.items())

    def clone(self):
        return ExternalContext(self.env, self.env_vars, self.logger, **self.kwargs)


class ExternalProgram(object):

    """Encapsulates an execution of an external program.

    Unlike subprocess, does not immediately execute the program but allows for further configuration
    and setup.  Converts keyword arguments in JSON into an argument list and allows for easy
    execution with custom environment variables.

    """

    def __init__(self,
                 executable,
                 context=None, env=None, env_vars=None,
                 custom_kwargs={},
                 **kwargs):

        self.executable = executable
        self.context = context
        if not self.context:
            self.context = ExternalContext(env, env_vars, **kwargs)
        else:
            self.context.kwargs.update(kwargs)

        self.custom_kwargs = custom_kwargs

        self.process = None

    def build_process(self, context=None):

        if not context:
            context = self.context

        process_kwargs = {}
        process_kwargs.update(context.kwargs)

        process = _Process(self.executable,
                           env_vars=context.env_vars,
                           logger=context.logger)

        apply_json_args(process, process_kwargs, self.custom_kwargs)

        return process

    def logger(self):
        return self.context.logger

    def start(self):
        self.process = self.build_process()
        self.process.start()

    def pid(self):
        return self.process.subprocess.pid

    def poll(self):
        return self.process.poll()

    def wait(self):
        return_code = self.process.wait()
        self.process = None
        return return_code

    def stop(self):
        return_code = self.process.stop()
        self.process = None
        return return_code

    def flush(self):
        self.process.flush()

    def __str__(self):
        return (self.process if self.process else self.build_process()).__str__()

    def __repr__(self):
        return self.__str__()


class _Process(object):

    """The system-independent execution of an external program.

    Handles finicky stuff once we have our environment, arguments, and logger sorted out.

    """

    def __init__(self, executable, arguments=[], env=None, env_vars=None, logger=None):

        self.executable = executable
        self.arguments = [] + arguments
        self.env = env
        self.env_vars = env_vars
        self.logger = logger

        self.subprocess = None
        self.stdout_logger = None
        self.stderr_logger = None
        # Windows only
        self.subprocess_job_object = None

    def start(self):

        argv, env = [self.executable] + self.arguments, self.env

        if self.env_vars:
            if not env:
                env = os.environ.copy()
            env.update(self.env_vars)

        creation_flags = 0
        if os.sys.platform == "win32":
            # Magic number needed to allow job reassignment in Windows 7
            # see: MSDN - Process Creation Flags - ms684863
            CREATE_BREAKAWAY_FROM_JOB = 0x01000000
            creation_flags = CREATE_BREAKAWAY_FROM_JOB

        stdout = sys.stdout if not self.logger else subprocess.PIPE
        stderr = sys.stderr if not self.logger else subprocess.PIPE

        self.subprocess = subprocess.Popen(argv, env=env, creationflags=creation_flags,
                                           stdout=stdout, stderr=stderr)

        if stdout == subprocess.PIPE:
            self.stdout_logger = LoggerPipe(self.logger, logging.INFO, self.subprocess.stdout)
            self.stdout_logger.wait_until_started()
        if stderr == subprocess.PIPE:
            self.stderr_logger = LoggerPipe(self.logger, logging.ERROR, self.subprocess.stderr)
            self.stderr_logger.wait_until_started()

        if os.sys.platform == "win32":

            # Create a job object with the "kill on job close" flag
            # This is inherited by child processes (i.e. the mongod started on our behalf by
            # buildlogger) and lets us terminate the whole tree of processes rather than
            # orphaning the mongod.
            import win32job

            job_object = win32job.CreateJobObject(None, '')

            job_info = win32job.QueryInformationJobObject(
                job_object,
                win32job.JobObjectExtendedLimitInformation)
            job_info['BasicLimitInformation']['LimitFlags'] |= \
                win32job.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
            win32job.SetInformationJobObject(job_object,
                                             win32job.JobObjectExtendedLimitInformation,
                                             job_info)
            win32job.AssignProcessToJobObject(job_object, proc._handle)

            self.subprocess_job_object = job_object

    def poll(self):
        return self.subprocess.poll()

    def wait(self):

        return_code = self.subprocess.wait()

        self.flush()
        if self.stdout_logger:
            self.stdout_logger.wait_until_finished()
            self.stdout_logger = None
        if self.stderr_logger:
            self.stderr_logger.wait_until_finished()
            self.stderr_logger = None

        return return_code

    def stop(self):

        try:
            if os.sys.platform == "win32":
                import win32job
                win32job.TerminateJobObject(self.subprocess_job_object, -1)
                # Windows doesn't seem to kill the process immediately, so give
                # it some time to die
                time.sleep(5)
            elif hasattr(self.subprocess, "terminate"):
                # This method added in Python 2.6
                self.subprocess.terminate()
            else:
                os.kill(self.subprocess.pid, 15)
        except Exception as e:
            print >> self.subprocess_outputs.stderr, "error shutting down process"
            print >> self.subprocess_outputs.stderr, e

        return self.wait()

    def flush(self):

        if self.subprocess:
            if not self.stderr_logger:
                # Going to the console
                sys.stderr.flush()
            else:
                self.stderr_logger.flush()

        if self.subprocess:
            if not self.stdout_logger:
                # Going to the console
                sys.stdout.flush()
            else:
                self.stdout_logger.flush()

    def __str__(self):

        # We only want to show the *different* environment variables
        def env_compare(env_orig, env_new):
            diff = {}
            for field, value in env_new.iteritems():
                if not field in env_orig:
                    diff[field] = value
            return diff

        env_diff = env_compare(os.environ, self.env) if self.env else {}
        if self.env_vars:
            for field, value in self.env_vars.iteritems():
                env_diff[field] = value

        env_strs = []
        for field, value in env_diff.iteritems():
            env_strs.append("%s=%s" % (field, value))

        cl = []
        if env_strs:
            cl.append(" ".join(env_strs))
        cl.append(self.executable)
        if self.arguments:
            cl.append(" ".join(self.arguments))
        if self.subprocess:
            cl.append("(%s)" % self.subprocess.pid)

        return " ".join(cl)

    def __repr__(self):
        return self.__str__()
