"""Unit tests for evergreen/functions/code_coverage_data_process.py."""

import importlib.util
import io
import tarfile
import textwrap
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from tempfile import TemporaryDirectory

# Loaded by path: the top-level "evergreen" directory is shadowed by the installed evergreen.py
# package, so "evergreen.functions.code_coverage_data_process" is not importable.
SCRIPT_PATH = (
    Path(__file__).resolve().parents[2]
    / "evergreen"
    / "functions"
    / "code_coverage_data_process.py"
)


def load_under_test():
    spec = importlib.util.spec_from_file_location("code_coverage_data_process", SCRIPT_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


under_test = load_under_test()
_parse_lcov_line_coverage = under_test._parse_lcov_line_coverage
_write_coverage_summary = under_test._write_coverage_summary
_archive_coverage_report = under_test._archive_coverage_report


class TmpDirTestCase(unittest.TestCase):
    def setUp(self):
        self._tmp = TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.tmpdir = Path(self._tmp.name)


class TestParseLcovLineCoverage(TmpDirTestCase):
    def parse(self, tracefile_contents: str) -> dict[str, dict[int, int]]:
        report = self.tmpdir / "coverage.dat"
        report.write_text(textwrap.dedent(tracefile_contents), encoding="utf-8")
        return _parse_lcov_line_coverage(str(report))

    def test_empty_report(self):
        self.assertEqual(self.parse(""), {})

    def test_single_record(self):
        per_file = self.parse("""\
            SF:src/mongo/db/foo.cpp
            DA:10,3
            DA:11,0
            DA:12,1
            end_of_record
            """)
        self.assertEqual(per_file, {"src/mongo/db/foo.cpp": {10: 3, 11: 0, 12: 1}})

    def test_multiple_files(self):
        per_file = self.parse("""\
            SF:a.cpp
            DA:1,1
            end_of_record
            SF:b.cpp
            DA:2,0
            end_of_record
            """)
        self.assertEqual(per_file, {"a.cpp": {1: 1}, "b.cpp": {2: 0}})

    def test_merged_records_accumulate_hits(self):
        """A file appearing in more than one record has its per-line hits summed, not clobbered."""
        per_file = self.parse("""\
            SF:a.cpp
            DA:1,2
            DA:2,0
            end_of_record
            SF:a.cpp
            DA:1,3
            DA:2,0
            DA:5,7
            end_of_record
            """)
        self.assertEqual(per_file, {"a.cpp": {1: 5, 2: 0, 5: 7}})

    def test_branch_and_function_records_ignored(self):
        per_file = self.parse("""\
            TN:
            SF:a.cpp
            FN:1,_Z3foov
            FNDA:4,_Z3foov
            FNF:1
            FNH:1
            BRDA:1,0,0,4
            BRF:1
            BRH:1
            DA:1,4
            LF:1
            LH:1
            end_of_record
            """)
        self.assertEqual(per_file, {"a.cpp": {1: 4}})

    def test_checksum_field_tolerated(self):
        per_file = self.parse("""\
            SF:a.cpp
            DA:1,4,vgX7Ma+A5RfaQFHK4YCFbg
            end_of_record
            """)
        self.assertEqual(per_file, {"a.cpp": {1: 4}})

    def test_malformed_da_records_skipped(self):
        per_file = self.parse("""\
            SF:a.cpp
            DA:1
            DA:notanumber,1
            DA:2,notanumber
            DA:3,9
            end_of_record
            """)
        self.assertEqual(per_file, {"a.cpp": {3: 9}})

    def test_da_outside_record_ignored(self):
        """DA: lines before any SF: or after end_of_record are dropped rather than crashing."""
        per_file = self.parse("""\
            DA:1,1
            SF:a.cpp
            DA:2,1
            end_of_record
            DA:3,1
            """)
        self.assertEqual(per_file, {"a.cpp": {2: 1}})

    def test_source_paths_are_stripped(self):
        per_file = self.parse("SF:  a.cpp  \nDA:1,1\nend_of_record\n")
        self.assertEqual(per_file, {"a.cpp": {1: 1}})

    def test_record_without_end_of_record(self):
        per_file = self.parse("SF:a.cpp\nDA:1,1\n")
        self.assertEqual(per_file, {"a.cpp": {1: 1}})


class TestWriteCoverageSummary(TmpDirTestCase):
    def summarize(self, tracefile_contents: str) -> tuple[str, str]:
        """Run _write_coverage_summary over a tracefile, returning (summary_text, stdout)."""
        self.report = self.tmpdir / "coverage.dat"
        self.report.write_text(textwrap.dedent(tracefile_contents), encoding="utf-8")
        summary = self.tmpdir / "summary.txt"
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            _write_coverage_summary(str(self.report), str(summary))
        return summary.read_text(encoding="utf-8"), stdout.getvalue()

    def expected_stdout(self, files: int, executable: int, covered: int, percent: str) -> str:
        return (
            f"[coverage-summary] files={files} executable_lines={executable} "
            f"covered_lines={covered} coverage={percent} "
            f"report={self.report} ({self.report.stat().st_size} bytes)\n"
            f"[coverage-summary] wrote per-file breakdown to {self.tmpdir / 'summary.txt'}\n"
        )

    def test_totals_and_per_file_rows(self):
        summary, stdout = self.summarize("""\
            SF:a.cpp
            DA:1,1
            DA:2,0
            DA:3,4
            end_of_record
            SF:b.cpp
            DA:1,0
            end_of_record
            """)
        self.assertEqual(
            summary,
            f"Coverage report: {self.report}\n"
            "Files: 2  Executable lines: 4  Covered lines: 2  Coverage: 50%\n"
            "\n"
            "   Lines     Exec  Uncovered    Cover  File\n"
            "       3        2          1      67%  a.cpp\n"
            "       1        0          1       0%  b.cpp\n"
            "\n"
            "       4        2          2      50%  TOTAL\n",
        )
        self.assertEqual(stdout, self.expected_stdout(2, 4, 2, "50%"))

    def test_files_sorted_by_path(self):
        summary, _ = self.summarize("""\
            SF:z.cpp
            DA:1,1
            end_of_record
            SF:m.cpp
            DA:1,1
            end_of_record
            SF:a.cpp
            DA:1,1
            end_of_record
            """)
        self.assertEqual(
            summary,
            f"Coverage report: {self.report}\n"
            "Files: 3  Executable lines: 3  Covered lines: 3  Coverage: 100%\n"
            "\n"
            "   Lines     Exec  Uncovered    Cover  File\n"
            "       1        1          0     100%  a.cpp\n"
            "       1        1          0     100%  m.cpp\n"
            "       1        1          0     100%  z.cpp\n"
            "\n"
            "       3        3          0     100%  TOTAL\n",
        )

    def test_empty_report_does_not_divide_by_zero(self):
        """An empty report is the diagnostic case this summary exists to distinguish."""
        summary, stdout = self.summarize("")
        self.assertEqual(
            summary,
            f"Coverage report: {self.report}\n"
            "Files: 0  Executable lines: 0  Covered lines: 0  Coverage: 0%\n"
            "\n"
            "   Lines     Exec  Uncovered    Cover  File\n"
            "\n"
            "       0        0          0       0%  TOTAL\n",
        )
        self.assertEqual(stdout, self.expected_stdout(0, 0, 0, "0%"))

    def test_hit_count_over_one_counts_line_once(self):
        summary, stdout = self.summarize("SF:a.cpp\nDA:1,500\nend_of_record\n")
        self.assertEqual(
            summary,
            f"Coverage report: {self.report}\n"
            "Files: 1  Executable lines: 1  Covered lines: 1  Coverage: 100%\n"
            "\n"
            "   Lines     Exec  Uncovered    Cover  File\n"
            "       1        1          0     100%  a.cpp\n"
            "\n"
            "       1        1          0     100%  TOTAL\n",
        )
        self.assertEqual(stdout, self.expected_stdout(1, 1, 1, "100%"))


class TestArchiveCoverageReport(TmpDirTestCase):
    def archive(self, contents: str) -> tuple[Path, Path, str]:
        """Archive a tracefile, returning (report, archive, stdout)."""
        report = self.tmpdir / "coverage.dat"
        report.write_text(contents, encoding="utf-8")
        archive = self.tmpdir / "coverage.tar.gz"
        with redirect_stdout(io.StringIO()) as stdout:
            _archive_coverage_report(str(report), str(archive))
        return report, archive, stdout.getvalue()

    def read_member(self, archive: Path) -> str:
        with tarfile.open(archive, "r:gz") as tar:
            self.assertEqual(tar.getnames(), [under_test.BAZEL_COVERAGE_REPORT_MEMBER])
            return tar.extractfile(under_test.BAZEL_COVERAGE_REPORT_MEMBER).read().decode()

    def test_roundtrips_report_contents(self):
        contents = "SF:a.cpp\nDA:1,1\nend_of_record\n" * 100
        report, archive, stdout = self.archive(contents)

        self.assertEqual(self.read_member(archive), contents)
        self.assertLess(archive.stat().st_size, report.stat().st_size)
        self.assertIn(f"archived raw lcov report to {archive}", stdout)

    def test_member_name_is_stable_not_the_bazel_output_path(self):
        """The entry is named for the artifact, not bazel's _coverage_report.dat."""
        _, archive, _ = self.archive("SF:a.cpp\nDA:1,1\nend_of_record\n")
        self.assertEqual(self.read_member(archive), "SF:a.cpp\nDA:1,1\nend_of_record\n")

    def test_empty_report(self):
        _, archive, _ = self.archive("")
        self.assertEqual(self.read_member(archive), "")


if __name__ == "__main__":
    unittest.main()
