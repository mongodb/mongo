"""Tests for reporting crashes recorded by C++ fuzz tests."""

import logging
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from buildscripts.resmokelib.testing.testcases import cpp_libfuzzer_test


class TestCPPLibfuzzerTestCase(unittest.TestCase):
    def setUp(self):
        self.temp_dir = self.enterContext(tempfile.TemporaryDirectory())
        self.corpus = Path(self.temp_dir) / "corpora"
        self.logger = logging.getLogger(self.id())
        self.testcase = cpp_libfuzzer_test.CPPLibfuzzerTestCase(
            self.logger, ["test_fuzzer"], corpus_directory_stem=str(self.corpus)
        )
        self.process = mock.Mock()
        self.process.wait.return_value = 0
        self.enterContext(
            mock.patch.object(self.testcase, "_make_process", return_value=self.process)
        )

    def _write_crash(self, test_name, input_id, binary="test_fuzzer"):
        path = self.corpus / binary / test_name / "crashing" / input_id
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b"fuzzing input")
        return path

    def _assert_crashes_fail(self, paths):
        with self.assertRaises(self.testcase.failureException) as failure:
            self.testcase.run_test()
        self.assertEqual(self.testcase.return_code, 1)
        self.assertIn(f"has {len(paths)} retained crash(es)", str(failure.exception))
        for path in paths:
            self.assertIn(str(path), str(failure.exception))
            self.assertEqual(path.read_bytes(), b"fuzzing input")

    def test_named_test_crash_fails_after_zero_exit(self):
        crash = self._write_crash("OrderedTicketSemaphoreFuzz.FuzzPriorityOrdering", "input-id")
        self._assert_crashes_fail([crash])

    def test_reports_crashes_from_every_test(self):
        named = self._write_crash("ExampleSuite.NamedTest", "input-id")
        llvm = self._write_crash("LLVMFuzzer.TestOneInput", "other-id")
        self._assert_crashes_fail([named, llvm])

    def test_passes_without_crashing_inputs(self):
        for test_name in ["LLVMFuzzer.TestOneInput", "ExampleSuite.NamedTest"]:
            (self.corpus / "test_fuzzer" / test_name / "crashing").mkdir(parents=True)
        seed = self.corpus / "test_fuzzer" / "ExampleSuite.NamedTest" / "seeds" / "seed-id"
        seed.parent.mkdir(parents=True)
        seed.write_bytes(b"fuzzing input")
        self._write_crash("ExampleSuite.NamedTest", "input-id", binary="other_fuzzer")

        self.testcase.run_test()
        self.assertEqual(self.testcase.return_code, 0)

    def test_nonzero_exit_preserves_failure(self):
        self.process.wait.return_value = 17
        self._write_crash("ExampleSuite.NamedTest", "input-id")

        with self.assertRaisesRegex(self.testcase.failureException, "exit code 17"):
            self.testcase.run_test()
        self.assertEqual(self.testcase.return_code, 17)


if __name__ == "__main__":
    unittest.main()
