"""Jasper logging handlers and helpers."""

try:
    import grpc
except ImportError:
    pass

import logging
import os
import time

from buildscripts.resmokelib import config
from buildscripts.resmokelib import utils


def get_test_log_url(group_id, test_id):
    """Return a URL to the test log in cedar."""

    return "https://{}/rest/v1/buildlogger/test_name/{}/{}/group/{}?execution={}".format(
        config.CEDAR_URL, config.EVERGREEN_TASK_ID, test_id, group_id, config.EVERGREEN_EXECUTION)


def get_logger_config(group_id=None, test_id=None, process_name=None, prefix=None, trial=None):
    # pylint: disable=too-many-locals
    """Return the jasper logger config."""

    import jasper.jasper_pb2 as pb

    username = os.getenv("CEDAR_USERNAME", default="")
    api_key = os.getenv("CEDAR_API_KEY", default="")
    test_id = utils.default_if_none(test_id, "")
    process_name = utils.default_if_none(process_name, "")
    prefix = utils.default_if_none(prefix, "")
    trial = utils.default_if_none(trial, 0)

    logger_config = pb.LoggerConfig()
    log_level = pb.LogLevel(threshold=30, default=30)
    log_format = pb.LogFormat.Value("LOGFORMATPLAIN")

    if config.EVERGREEN_TASK_ID and group_id is not None:
        buildlogger_info = pb.BuildloggerV3Info(
            project=config.EVERGREEN_PROJECT_NAME, version=config.EVERGREEN_VERSION_ID,
            variant=config.EVERGREEN_VARIANT_NAME, task_name=config.EVERGREEN_TASK_NAME,
            task_id=config.EVERGREEN_TASK_ID, execution=config.EVERGREEN_EXECUTION,
            test_name=str(test_id), proc_name=process_name, trial=trial, format=log_format, tags=[
                str(group_id)
            ], prefix=prefix, base_address=config.CEDAR_URL, rpc_port=config.CEDAR_RPC_PORT,
            username=username, api_key=api_key)
        buildlogger_options = pb.BuildloggerV3Options(buildloggerv3=buildlogger_info,
                                                      level=log_level)
        logger_config.buildloggerv3.CopyFrom(buildlogger_options)
    else:
        buffered = pb.BufferOptions()
        base_opts = pb.BaseOptions(format=log_format, level=log_level, buffer=buffered)
        log_opts = pb.DefaultLoggerOptions(prefix=prefix, base=base_opts)
        logger_config.default.CopyFrom(log_opts)

    return logger_config


class JasperHandler(logging.Handler):
    """Jasper logger handler."""

    pb = None
    rpc = None

    def __init__(self, name, group_id, test_id=None):
        """Initialize the jasper handler.

        The name uniquely identifies the logger within the logging cache. Group
        id is required for cedar buildlogger, test id and trial are optional.
        """
        logging.Handler.__init__(self)
        self.name = "{}-{}".format(name, time.monotonic())
        self.closed = False

        logger_config = get_logger_config(group_id=group_id, test_id=test_id,
                                          process_name=self.name)
        output_opts = self.pb.OutputOptions(loggers=[logger_config])
        create_args = self.pb.LoggingCacheCreateArgs(id=self.name, options=output_opts)

        self.stub = self.rpc.JasperProcessManagerStub(
            grpc.insecure_channel(config.JASPER_CONNECTION_STR))
        try:
            instance = self.stub.LoggingCacheCreate(create_args)
            if not instance.outcome.success:
                raise RuntimeError("Failed to setup jasper handler: {}".format(
                    instance.outcome.text))
        except grpc.RpcError as rpc_err:
            raise RuntimeError("Failed to setup jasper handler with status code {}: {}".format(
                rpc_err.code(), rpc_err.details()))

    def emit(self, record):
        """Emit a record to the jasper logging backend."""
        record = self.format(record)

        log_format = self.pb.LoggingPayloadFormat.Value("FORMATSTRING")
        log_data = self.pb.LoggingPayloadData()
        log_data.msg = record
        logging_payload = self.pb.LoggingPayload(LoggerID=self.name, priority=40, format=log_format,
                                                 data=[log_data])
        try:
            outcome = self.stub.SendMessages(logging_payload)
            if not outcome.success:
                raise RuntimeError("Failed to send log message via jasper: {}".format(outcome.text))
        except grpc.RpcError as rpc_err:
            raise RuntimeError(
                "Failed to send log message via jasper with status code {}: {}".format(
                    rpc_err.code(), rpc_err.details()))

    def close(self):
        """Close the logging handler."""
        logging.Handler.close(self)

        if not self.closed:
            self.closed = True

            args = self.pb.LoggingCacheArgs(id=self.name)
            try:
                outcome = self.stub.LoggingCacheCloseAndRemove(args)
                if not outcome.success:
                    raise RuntimeError("Failed to close jasper logger: {}".format(outcome.text))
            except grpc.RpcError as rpc_err:
                raise RuntimeError("Failed to close jasper logger with status code {}: {}".format(
                    rpc_err.code(), rpc_err.details()))
