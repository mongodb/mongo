"""A process management system using mongodb/jasper.

Serves as an alternative to process.py.
"""

try:
    import grpc
except ImportError:
    pass

from buildscripts.resmokelib import errors
from buildscripts.resmokelib.core import process as _process
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface


class Process(_process.Process):
    """Class for spawning a process using mongodb/jasper."""

    jasper_pb2 = None
    jasper_pb2_grpc = None
    connection_str = None

    def __init__(self, logger, args, env=None, env_vars=None):
        """Initialize the process with the specified logger, arguments, and environment."""
        _process.Process.__init__(self, logger, args, env=env, env_vars=env_vars)
        self._id = None
        self._stub = self.jasper_pb2_grpc.JasperProcessManagerStub(
            grpc.insecure_channel(self.connection_str))
        self._return_code = None

    def start(self):
        """Start the process and the logger pipes for its stdout and stderr."""
        log_format = self.jasper_pb2.LogFormat.Value("LOGFORMATPLAIN")
        log_level = self.jasper_pb2.LogLevel()
        buffered = self.jasper_pb2.BufferOptions()
        base_opts = self.jasper_pb2.BaseOptions(format=log_format, level=log_level, buffer=buffered)
        log_opts = self.jasper_pb2.InheritedLoggerOptions(base=base_opts)
        logger = self.jasper_pb2.LoggerConfig()
        logger.inherited.CopyFrom(log_opts)

        output_opts = self.jasper_pb2.OutputOptions(loggers=[logger])
        create_options = self.jasper_pb2.CreateOptions(
            args=self.args,
            environment=self.env,
            override_environ=True,
            timeout_seconds=0,
            output=output_opts,
        )

        val = self._stub.Create(create_options)
        self.pid = val.pid
        self._id = self.jasper_pb2.JasperProcessID(value=val.id)
        self._return_code = None

    def stop(self, mode=None):
        """Terminate the process."""
        if mode is None:
            mode = fixture_interface.TeardownMode.TERMINATE

        if mode == fixture_interface.TeardownMode.KILL:
            signal = self.jasper_pb2.Signals.Value("KILL")
        elif mode == fixture_interface.TeardownMode.TERMINATE:
            signal = self.jasper_pb2.Signals.Value("TERMINATE")
        elif mode == fixture_interface.TeardownMode.ABORT:
            signal = self.jasper_pb2.Signals.Value("ABRT")
        else:
            raise errors.ProcessError("Process wrapper given unrecognized teardown mode: " +
                                      mode.value)

        signal_process = self.jasper_pb2.SignalProcess(ProcessID=self._id, signal=signal)
        val = self._stub.Signal(signal_process)
        if not val.success \
                and "cannot signal a process that has terminated" not in val.text \
                and "os: process already finished" not in val.text:
            raise OSError("Failed to signal Jasper process with pid {}: {}".format(
                self.pid, val.text))

    def poll(self):
        """Poll."""
        if self._return_code is None:
            process = self._stub.Get(self._id)
            if not process.running:
                self.wait()
        return self._return_code

    def wait(self, timeout=None):
        """Wait until process has terminated and all output has been consumed by the logger pipes."""
        if self._return_code is None:
            wait = self._stub.Wait(self._id)
            if not wait.success:
                raise OSError("Failed to wait on process with pid {}: {}.".format(
                    self.pid, wait.text))
            self._return_code = wait.exit_code
        return self._return_code
