"""Unit tests for builscripts/resmokelib/jasper.proto."""

import os
import subprocess
import sys
import time
import unittest

from buildscripts.resmokelib import config
from buildscripts.resmokelib import run

# pylint: disable=missing-docstring,protected-access,too-many-locals


class TestJasperLogging(unittest.TestCase):
    """Unit test for the cedar logging endpoint with the jasper.proto file."""

    def test_logging_endpoint(self):
        if not config.EVERGREEN_TASK_ID or sys.platform == "x86_64":
            print(
                "Testing jasper logging endpoint should only be tested in Evergreen on non-x86_64 platforms, skipping..."
            )
            return

        import grpc
        from google.protobuf import empty_pb2

        runner = run.TestRunner("")
        curator_path = runner._get_jasper_reqs()

        import jasper.jasper_pb2 as pb
        import jasper.jasper_pb2_grpc as rpc

        jasper_port = 8000
        jasper_conn_str = "localhost:%d" % jasper_port
        jasper_command = [
            curator_path, "jasper", "service", "run", "rpc", "--port",
            str(jasper_port)
        ]
        jasper_service = subprocess.Popen(jasper_command)
        time.sleep(1)
        stub = rpc.JasperProcessManagerStub(grpc.insecure_channel(jasper_conn_str))

        level = pb.LogLevel(threshold=30, default=30)
        buildlogger_info = pb.BuildloggerV3Info(
            project=config.EVERGREEN_PROJECT_NAME, version=config.EVERGREEN_VERSION_ID,
            variant=config.EVERGREEN_VARIANT_NAME, task_name=config.EVERGREEN_TASK_NAME,
            task_id=config.EVERGREEN_TASK_ID, test_name="test-jasper-proto",
            execution=config.EVERGREEN_EXECUTION, base_address="cedar.mongodb.com", rpc_port="7070",
            username=os.getenv("CEDAR_USER"), api_key=os.getenv("CEDAR_API_KEY"))
        buildlogger_options = pb.BuildloggerV3Options(buildloggerv3=buildlogger_info, level=level)
        logger_config = pb.LoggerConfig()
        logger_config.buildloggerv3.CopyFrom(buildlogger_options)
        create_options = pb.CreateOptions(args=["ls"], working_directory='.',
                                          output=pb.OutputOptions(loggers=[logger_config]))
        res = stub.Create(request=create_options)
        self.assertEqual(0, res.exit_code)
        res = stub.Close(empty_pb2.Empty())
        self.assertTrue(res.success)

        jasper_service.terminate()
