"""Compare Cedar data."""

from dataclasses import dataclass, field
from enum import Enum
from typing import Union, List

from buildscripts.benchmarks.config import get_thresholds
from buildscripts.util.cedar_api import CedarPerfData


class _Result(Enum):
    """Individual benchmark test result."""

    SUCCESS = "passed"
    FAIL = "failed"


@dataclass
class _ResultTableRow:
    """Representation of result table row."""

    test_name: str
    thread_level: int
    metric_name: str
    baseline_value: Union[int, float]
    value: Union[int, float]
    percent_delta: Union[int, float]
    percent_threshold: Union[int, float]
    result: str

    @classmethod
    def get_headers(cls) -> List[str]:
        """Return headers of the result table."""
        return [
            "Test Name",
            "Thread Level",
            "Metric Name",
            "Baseline Value(ns)",
            "Value(ns)",
            "Delta(%)",
            "Threshold(%)",
            "Result",
        ]

    def as_list_of_str(self) -> List[str]:
        """Return list of strings representation of result row."""
        return [
            self.test_name,
            f"{self.thread_level}",
            self.metric_name,
            "" if self.baseline_value is None else f"{self.baseline_value:.2f}",
            "" if self.value is None else f"{self.value:.2f}",
            f"{self.percent_delta * 100:.2f}%",
            f"{self.percent_threshold * 100:.2f}%",
            self.result,
        ]


@dataclass
class _ResultTable:
    """Representation of result table."""

    rows: List[List[str]] = field(default_factory=lambda: [])
    widths: List[int] = field(
        default_factory=lambda: [len(value) for value in _ResultTableRow.get_headers()])
    passed: bool = True

    def add_result(self, entry: _ResultTableRow) -> None:
        """Add result row to the result table."""
        self.rows.append(entry.as_list_of_str())

    def __str__(self) -> str:
        """Return string representation of the result table."""
        self._count_widths()
        sep = self._make_separator()

        table = f"{sep}\n{self._make_row_str(_ResultTableRow.get_headers())}\n{sep}"
        for row in self.rows:
            table = f"{table}\n{self._make_row_str(row)}"
        table = f"{table}\n{sep}"

        return table

    def _count_widths(self) -> None:
        for row in self.rows:
            for i, value in enumerate(row):
                self.widths[i] = max(self.widths[i], len(value))

    def _make_separator(self) -> str:
        sep = "+".join(f"-{'-' * width}-" for width in self.widths)
        return f"+{sep}+"

    def _make_row_str(self, row: List[str]) -> str:
        row_str = "|".join(
            f" {value}{' ' * (width - len(value))} " for width, value in zip(self.widths, row))
        return f"|{row_str}|"


def compare_data(suite: str, current: List[CedarPerfData],
                 baseline: List[CedarPerfData]) -> _ResultTable:
    """Compare the current performance data with the baseline data."""
    result_table = _ResultTable()

    thresholds = get_thresholds(suite)
    baseline_dict = {
        f"{test.test_name}_{test.thread_level}_{rollup.name}": rollup.val
        for test in baseline for rollup in test.perf_rollups
    }

    for test in current:
        for rollup in test.perf_rollups:
            baseline_val = baseline_dict.get(f"{test.test_name}_{test.thread_level}_{rollup.name}",
                                             None)
            percent_delta = 0
            if baseline_val:
                percent_delta = (rollup.val - baseline_val) / baseline_val

            metric_name = rollup.name
            if rollup.name.rsplit("_", 1)[1].isdigit():
                metric_name = rollup.name.rsplit("_", 1)[0]
            percent_threshold = thresholds[metric_name]

            result = _Result.SUCCESS.value
            if percent_delta > percent_threshold:
                result = _Result.FAIL.value
                result_table.passed = False

            result_table.add_result(
                _ResultTableRow(test_name=test.test_name, thread_level=test.thread_level,
                                metric_name=rollup.name, baseline_value=baseline_val,
                                value=rollup.val, percent_delta=percent_delta,
                                percent_threshold=percent_threshold, result=result))

    return result_table
