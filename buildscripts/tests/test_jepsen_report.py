"""Tests for jepsen report generator."""

import os
import random
import textwrap
import unittest
from unittest.mock import patch

from click.testing import CliRunner

from buildscripts.jepsen_report import ParserOutput, main, parse

_CORPUS = textwrap.dedent("""\
     "indeterminate: Command failed with error 251 (NoSuchTransaction): 'Transaction was aborted :: caused by :: from shard rs_shard2 :: caused by :: Given transaction number 53 does not match any in-progress transactions. The active transaction number is -1' on server n9:27017. The full response is {\"writeConcernError\": {\"code\": 6, \"codeName\": \"HostUnreachable\", \"errmsg\": \"operation was interrupted\", \"errInfo\": {\"writeConcern\": {\"w\": \"majority\", \"wtimeout\": 0, \"provenance\": \"clientSupplied\"}}}, \"topologyVersion\": {\"processId\": {\"$oid\": \"625f0b0ef9d6a12d9b562ff9\"}, \"counter\": 21}, \"ok\": 0.0, \"errmsg\": \"Transaction was aborted :: caused by :: from shard rs_shard2 :: caused by :: Given transaction number 53 does not match any in-progress transactions. The active transaction number is -1\", \"code\": 251, \"codeName\": \"NoSuchTransaction\", \"$clusterTime\": {\"clusterTime\": {\"$timestamp\": {\"t\": 1650395950, \"i\": 16}}, \"signature\": {\"hash\": {\"$binary\": {\"base64\": \"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\", \"subType\": \"00\"}}, \"keyId\": 0}}, \"operationTime\": {\"$timestamp\": {\"t\": 1650395950, \"i\": 5}}, \"recoveryToken\": {\"recoveryShardId\": \"rs_shard1\"}}",
     :index 1141}})},
 :workload {:valid? true},
 :valid? true}


Everything looks good! ヽ(‘ー`)ノ



# Successful tests

store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T163539.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T164131.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T164724.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T165317.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T165910.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T170503.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T171055.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T171648.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T172239.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T172832.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T173634.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T174227.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T174820.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T175413.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T180006.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T180558.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T181148.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T181946.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T182539.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T183131.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T183720.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T184313.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T184906.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T185459.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T190052.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T190645.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T191239.000Z
store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20220419T191833.000Z

# Crashed tests

mongodb list-append w:majority r:majority tw:majority tr:snapshot partition
mongodb list-append w:majority r:majority tw:majority tr:snapshot partition

28 successes
0 unknown
2 crashed
0 failures
""").splitlines()


class TestParser(unittest.TestCase):
    """TestParser."""

    @classmethod
    def _corpus_generator(cls):
        n_pass = random.randint(1, 30)
        n_fail = random.randint(1, 30)
        n_unknown = random.randint(1, 30)
        n_crash = random.randint(1, 30)

        corpus = textwrap.dedent("""\
     :index 2893}})},
 :workload {:valid? true},
 :valid? true}
Everything looks good! ヽ(‘ー`)ノ
""")
        successful_tests = []
        if n_pass > 0:
            corpus += "\n# Successful tests\n\n"
            for i in range(0, n_pass):
                corpus += f"test {i}\n"
                successful_tests.append(f"test {i}")

        failed_tests = []
        if n_fail > 0:
            corpus += "\n# Failed tests\n\n"
            for i in range(0, n_fail):
                corpus += f"test {i}\n"
                failed_tests.append(f"test {i}")

        indeterminate_tests = []
        if n_unknown > 0:
            corpus += "\n# Indeterminate tests\n\n"
            for i in range(0, n_unknown):
                corpus += f"test {i}\n"
                indeterminate_tests.append(f"test {i}")

        crashed_tests = []
        if n_crash > 0:
            corpus += "\n# Crashed tests\n\n"
            for i in range(0, n_crash):
                corpus += f"test {i}\n"
                crashed_tests.append(f"test {i}")
        # note leading newline for this block is required to match the actual
        # logs
        corpus += textwrap.dedent(f"""
{n_pass} successes
{n_unknown} unknown
{n_crash} crashed
{n_fail} failures
""")

        return {
            "expected": ParserOutput(
                {
                    "success": successful_tests,
                    "unknown": indeterminate_tests,
                    "crashed": crashed_tests,
                    "failed": failed_tests,
                }
            ),
            "corpus": corpus,
        }

    def test_parser(self):
        """Test with embedded corpus."""
        out = parse(_CORPUS)
        self.assertEqual(len(out["success"]), 28)
        self.assertEqual(len(out["unknown"]), 0)
        self.assertEqual(len(out["crashed"]), 2)
        self.assertEqual(len(out["failed"]), 0)

    def test_parser2(self):
        """Test with jepsen.log file."""
        with open(
            os.path.join(os.path.dirname(__file__), "test_jepsen_report_corpus.log.txt")
        ) as fh:
            corpus = fh.read().splitlines()
        out = parse(corpus)
        self.assertEqual(len(out["success"]), 29)
        self.assertEqual(len(out["unknown"]), 0)
        self.assertEqual(len(out["crashed"]), 1)
        self.assertEqual(len(out["failed"]), 0)

    def test_generated_corpus(self):
        """Generate 100 corpuses and test them."""
        for _ in range(0, 100):
            self._test_generated_corpus()

    def _test_generated_corpus(self):
        gen = self._corpus_generator()
        corpus = gen["corpus"].splitlines()
        out = parse(corpus)
        self.assertDictEqual(out, gen["expected"])

    @patch("buildscripts.jepsen_report._try_find_log_file")
    @patch("buildscripts.jepsen_report._get_log_lines")
    @patch("buildscripts.jepsen_report._put_report")
    def test_main(self, mock_put_report, mock_get_log_lines, mock_try_find_log_file):
        """Test main function."""
        gen = self._corpus_generator()
        corpus = gen["corpus"].splitlines()
        mock_get_log_lines.return_value = corpus

        def _try_find_log_file(_store, _test):
            if _try_find_log_file.counter == 0:
                _try_find_log_file.counter += 1
                with open(
                    os.path.join(os.path.dirname(__file__), "test_jepsen_report_corpus.log.txt")
                ) as fh:
                    return fh.read()
            return ""

        _try_find_log_file.counter = 0
        mock_try_find_log_file.side_effect = _try_find_log_file

        runner = CliRunner()
        result = runner.invoke(
            main, ["--start_time=0", "--end_time=10", "--elapsed=10", "test.log"]
        )
        num_tests = (
            len(gen["expected"]["success"])
            + len(gen["expected"]["unknown"])
            + len(gen["expected"]["crashed"])
            + len(gen["expected"]["failed"])
        )
        num_fails = num_tests - len(gen["expected"]["success"])

        callee_dict = mock_put_report.call_args[0][0]
        self.assertEqual(callee_dict["failures"], num_fails)
        self.assertEqual(len(callee_dict["results"]), num_tests)
        mock_get_log_lines.assert_called_once_with("test.log")
        if gen["expected"]["crashed"]:
            self.assertEqual(result.exit_code, 2)
        elif gen["expected"]["unknown"] or gen["expected"]["failure"]:
            self.assertEqual(result.exit_code, 1)
        else:
            self.assertEqual(result.exit_code, 0)
