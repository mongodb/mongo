"""A process management system using mongodb/jasper.

Serves as an alternative to process.py.
"""

try:
    import grpc
except ImportError:
    pass

from buildscripts.resmokelib import config
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.core import process as _process
from buildscripts.resmokelib.logging.jasper_logger import get_logger_config
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface


class Process(_process.Process):
    """Class for spawning a process using mongodb/jasper."""

    pb = None
    rpc = None

    def __init__(self, logger, args, env=None, env_vars=None, job_num=None, test_id=None):  # pylint: disable=too-many-arguments
        """Initialize the process with the specified logger, arguments, and environment."""
        _process.Process.__init__(self, logger, args, env=env, env_vars=env_vars)
        self._id = None
        self.job_num = job_num
        self.test_id = test_id
        self._stub = self.rpc.JasperProcessManagerStub(
            grpc.insecure_channel(config.JASPER_CONNECTION_STR))
        self._return_code = None

    def start(self):
        """Start the process and the logger pipes for its stdout and stderr."""
        logger = get_logger_config(group_id=self.job_num, test_id=self.test_id,
                                   process_name=self.args[0])
        output_opts = self.pb.OutputOptions(loggers=[logger])
        create_options = self.pb.CreateOptions(
            args=self.args,
            environment=self.env,
            override_environ=True,
            timeout_seconds=0,
            output=output_opts,
        )

        val = self._stub.Create(create_options)
        self.pid = val.pid
        self._id = self.pb.JasperProcessID(value=val.id)
        self._return_code = None

    def stop(self, mode=None):
        """Terminate the process."""
        if mode is None:
            mode = fixture_interface.TeardownMode.TERMINATE

        if mode == fixture_interface.TeardownMode.KILL:
            signal = self.pb.Signals.Value("KILL")
        elif mode == fixture_interface.TeardownMode.TERMINATE:
            signal = self.pb.Signals.Value("TERMINATE")
        elif mode == fixture_interface.TeardownMode.ABORT:
            signal = self.pb.Signals.Value("ABRT")
        else:
            raise errors.ProcessError("Process wrapper given unrecognized teardown mode: " +
                                      mode.value)

        signal_process = self.pb.SignalProcess(ProcessID=self._id, signal=signal)
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
