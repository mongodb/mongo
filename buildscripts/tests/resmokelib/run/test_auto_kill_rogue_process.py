"""Unit tests for buildscripts/resmokelib/run/list_tags.py."""
# pylint: disable=protected-access
import unittest
import logging
import os
import sys

import psutil

from buildscripts.resmokelib.run import TestRunner
from buildscripts.resmokelib import errors
from buildscripts.resmokelib.core import process
from buildscripts.resmokelib.testing.fixtures import interface as fixture_interface
import buildscripts.resmokelib.config


class MockTestRunner(TestRunner):
    def _setup_logging(self):
        self._exec_logger = logging.getLogger()
        self._exec_logger.addHandler(logging.NullHandler())
        self._resmoke_logger = self._exec_logger


class TestDetectRogueProcess(unittest.TestCase):
    def setUp(self) -> None:
        self.command = [sys.executable, '-c', "import time; time.sleep(5)"]
        if sys.platform.lower() == 'win32':
            self.sigkill_return = fixture_interface.TeardownMode.TERMINATE.value
        else:
            self.sigkill_return = -fixture_interface.TeardownMode.KILL.value

        if not os.environ.get('RESMOKE_PARENT_PROCESS'):
            os.environ['RESMOKE_PARENT_PROCESS'] = str(os.getpid())
            os.environ['RESMOKE_PARENT_CTIME'] = str(psutil.Process().create_time())

    def test_warn(self):
        buildscripts.resmokelib.config.AUTO_KILL = 'warn'
        buildscripts.resmokelib.config.SHELL_CONN_STRING = None

        test_runner = MockTestRunner("test")
        test_runner._setup_logging()

        try:
            test_runner._check_for_mongo_processes()
        except errors.ResmokeError:
            self.fail("Detected processes when there should be none.")

        tmp_ctime = os.environ['RESMOKE_PARENT_CTIME']
        os.environ['RESMOKE_PARENT_CTIME'] = str("rogue_process")
        proc = process.Process(logging.getLogger(), self.command)
        proc.start()
        os.environ['RESMOKE_PARENT_CTIME'] = tmp_ctime

        with self.assertRaises(errors.ResmokeError):
            test_runner._check_for_mongo_processes()

        proc.stop(mode=fixture_interface.TeardownMode.KILL)
        proc.wait()

    def test_on(self):

        buildscripts.resmokelib.config.AUTO_KILL = 'on'
        buildscripts.resmokelib.config.SHELL_CONN_STRING = None

        test_runner = MockTestRunner("test")
        test_runner._setup_logging()

        test_runner._check_for_mongo_processes()

        tmp_ctime = os.environ['RESMOKE_PARENT_CTIME']
        os.environ['RESMOKE_PARENT_CTIME'] = str("rogue_process")
        proc = process.Process(logging.getLogger(), self.command)
        proc.start()
        os.environ['RESMOKE_PARENT_CTIME'] = tmp_ctime

        test_runner._check_for_mongo_processes()

        proc.wait()

        if proc._process.returncode != self.sigkill_return:
            self.fail(
                f"Detected processes was not killed by resmoke, exit code was {proc._process.returncode}, expected {self.sigkill_return}"
            )

    def test_off(self):
        buildscripts.resmokelib.config.AUTO_KILL = 'off'
        buildscripts.resmokelib.config.SHELL_CONN_STRING = None

        test_runner = MockTestRunner("test")
        test_runner._setup_logging()

        test_runner._check_for_mongo_processes()

        proc = process.Process(logging.getLogger(), self.command)
        proc.start()

        test_runner._check_for_mongo_processes()

        proc.stop(mode=fixture_interface.TeardownMode.ABORT)

        if proc._process.returncode == self.sigkill_return:
            self.fail("Process was killed when it should not have been.")
        proc.wait()

    def test_shell_constring(self):
        buildscripts.resmokelib.config.AUTO_KILL = 'warn'
        buildscripts.resmokelib.config.SHELL_CONN_STRING = '127.0.0.1:27000'

        test_runner = MockTestRunner("test")
        test_runner._setup_logging()

        test_runner._check_for_mongo_processes()

        proc = process.Process(logging.getLogger(), self.command)
        proc.start()

        test_runner._check_for_mongo_processes()

        proc.stop(mode=fixture_interface.TeardownMode.ABORT)

        if proc._process.returncode == self.sigkill_return:
            self.fail("Process was killed when it should not have been.")

        proc.wait()
