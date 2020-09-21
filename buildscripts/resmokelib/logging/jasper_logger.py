"""Jasper logging handlers and helpers."""

import os

from buildscripts.resmokelib import config


def get_logger_config(group_id="", test_id="", process_name=""):
    """Return the jasper logger config."""

    import jasper.jasper_pb2 as pb

    username = os.getenv("CEDAR_USERNAME", default="")
    api_key = os.getenv("CEDAR_API_KEY", default="")

    logger_config = pb.LoggerConfig()
    log_level = pb.LogLevel(threshold=30, default=30)
    log_format = pb.LogFormat.Value("LOGFORMATPLAIN")

    if config.EVERGREEN_TASK_ID and group_id:
        buildlogger_info = pb.BuildloggerV3Info(
            project=config.EVERGREEN_PROJECT_NAME, version=config.EVERGREEN_VERSION_ID,
            variant=config.EVERGREEN_VARIANT_NAME, task_name=config.EVERGREEN_TASK_NAME,
            task_id=config.EVERGREEN_TASK_ID, execution=config.EVERGREEN_EXECUTION,
            test_name=str(test_id), process_name=process_name, format=log_format, tags=[
                str(group_id)
            ], base_address=config.CEDAR_URL, rpc_port=config.CEDAR_RPC_PORT, username=username,
            api_key=api_key)
        buildlogger_options = pb.BuildloggerV3Options(buildloggerv3=buildlogger_info,
                                                      level=log_level)
        logger_config.buildloggerv3.CopyFrom(buildlogger_options)
    else:
        buffered = pb.BufferOptions()
        base_opts = pb.BaseOptions(format=log_format, level=log_level, buffer=buffered)
        log_opts = pb.DefaultLoggerOptions(base=base_opts)
        logger_config.default.CopyFrom(log_opts)

    return logger_config
