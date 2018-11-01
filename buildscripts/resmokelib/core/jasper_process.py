"""A process management system using mongodb/jasper.

Serves as an alternative to process.py.
"""

from __future__ import absolute_import

try:
    import grpc
except ImportError:
    pass

from . import process as _process


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
        log_type = self.jasper_pb2.LogType.Value("LOGINHERIT")
        log_format = self.jasper_pb2.LogFormat.Value("LOGFORMATPLAIN")
        log_options = self.jasper_pb2.LogOptions(format=log_format)
        logger = self.jasper_pb2.Logger(log_type=log_type, log_options=log_options)

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

    def stop(self, kill=False):
        """Terminate the process."""
        signal = self.jasper_pb2.Signals.Value("TERMINATE")
        if kill:
            signal = self.jasper_pb2.Signals.Value("KILL")

        signal_process = self.jasper_pb2.SignalProcess(ProcessID=self._id, signal=signal)
        try:
            val = self._stub.Signal(signal_process)
            if not val.success:
                raise OSError("Unable to stop process %d." % self.pid)
        except grpc.RpcError as err:
            err.details = err.details()
            if "cannot signal a process that has terminated" not in err.details \
                    and "os: process already finished" not in err.details:
                raise

    def poll(self):
        """Poll."""
        if self._return_code is None:
            process = self._stub.Get(self._id)
            if not process.running:
                self.wait()
        return self._return_code

    def wait(self):
        """Wait until process has terminated and all output has been consumed by the logger pipes."""
        if self._return_code is None:
            try:
                wait = self._stub.Wait(self._id)
                self._return_code = wait.exit_code
            except grpc.RpcError as err:
                if "problem encountered while waiting: operation failed" not in err.details():
                    raise
                wait = self._stub.Get(self._id)
                self._return_code = wait.exit_code
        return self._return_code
