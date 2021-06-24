"""Unit tests for buildscripts/resmokelib/core/jasper_process.py."""
# pylint: disable=missing-docstring,protected-access
import logging
import unittest

from mock import MagicMock

from buildscripts.resmokelib import config
from buildscripts.resmokelib import run
from buildscripts.resmokelib.core import jasper_process as test_jasper_process


class TestJasperPids(unittest.TestCase):
    resmoke_runner: run.TestRunner

    @classmethod
    def setUpClass(cls) -> None:
        resmoke_config = config
        resmoke_config.BASE_PORT = 20000
        run.config = resmoke_config
        test_jasper_process.config = resmoke_config
        run.jasper_process = test_jasper_process
        cls.resmoke_runner = run.TestRunner("jasper")
        cls.resmoke_runner._resmoke_logger = MagicMock()
        cls.resmoke_runner._setup_jasper()
        cls.orig_jasper_pids = test_jasper_process.JASPER_PIDS.copy()

    @classmethod
    def tearDownClass(cls) -> None:
        cls.resmoke_runner._exit_jasper()

    def setUp(self) -> None:
        test_jasper_process.JASPER_PIDS = self.orig_jasper_pids.copy()

    def test_pid_is_recorded(self) -> None:
        test_pid = "1111"
        test_proc = test_jasper_process.Process(
            logging.Logger("jasper"), ["jasper"], job_num=1, test_id=1)
        test_proc._stub.Create = MagicMock(return_value=MagicMock(pid=test_pid, id="1"))
        test_proc.start()
        self.assertEqual(test_pid, test_proc.pid)
        self.assertEqual(len(test_jasper_process.JASPER_PIDS), 1)
        self.assertTrue(test_pid in test_jasper_process.JASPER_PIDS)

    def test_pids_are_recorded(self) -> None:
        test_pids = ["1111", "2222", "3333"]
        for pid in test_pids:
            test_proc = test_jasper_process.Process(
                logging.Logger("jasper"), ["jasper"], job_num=1, test_id=1)
            test_proc._stub.Create = MagicMock(return_value=MagicMock(pid=pid, id="1"))
            test_proc.start()
        self.assertEqual(len(test_pids), len(test_jasper_process.JASPER_PIDS))
        self.assertTrue(all(pid in test_jasper_process.JASPER_PIDS for pid in test_pids))

    def test_pid_is_removed_by_stop(self) -> None:
        pids = ["1111", "2222", "3333"]
        procs = []
        for pid in pids:
            proc = test_jasper_process.Process(
                logging.Logger("jasper"), ["jasper"], job_num=1, test_id=1)
            proc._stub.Create = MagicMock(return_value=MagicMock(pid=pid, id="1"))
            proc.start()
            procs.append(proc)

        orig_recorded_num_pids = len(test_jasper_process.JASPER_PIDS)
        test_proc = procs[1]
        self.assertTrue(test_proc.pid in test_jasper_process.JASPER_PIDS)
        test_proc._stub.Signal = MagicMock(return_value=MagicMock(success=True))
        test_proc.stop()
        self.assertFalse(test_proc.pid in test_jasper_process.JASPER_PIDS)
        self.assertEqual(orig_recorded_num_pids - len(test_jasper_process.JASPER_PIDS), 1)

    def test_pid_is_removed_by_wait(self) -> None:
        pids = ["1111", "2222", "3333"]
        procs = []
        for pid in pids:
            proc = test_jasper_process.Process(
                logging.Logger("jasper"), ["jasper"], job_num=1, test_id=1)
            proc._stub.Create = MagicMock(return_value=MagicMock(pid=pid, id="1"))
            proc.start()
            procs.append(proc)

        orig_recorded_num_pids = len(test_jasper_process.JASPER_PIDS)
        test_proc = procs[1]
        self.assertTrue(test_proc.pid in test_jasper_process.JASPER_PIDS)
        test_proc._stub.Wait = MagicMock(return_value=MagicMock(exit_code=0))
        test_proc.wait()
        self.assertFalse(test_proc.pid in test_jasper_process.JASPER_PIDS)
        self.assertEqual(orig_recorded_num_pids - len(test_jasper_process.JASPER_PIDS), 1)
