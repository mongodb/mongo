"""Generate Evergreen reports from the Jepsen list-append workload."""

import json
import os
import re
import sys
from datetime import datetime, timezone
from typing import List, Optional, Tuple

import click
from typing_extensions import TypedDict

from buildscripts.simple_report import Report, Result


class ParserOutput(TypedDict):
    """Result of parsing jepsen log file. Each List[str] is a list of test names."""

    success: List[str]
    unknown: List[str]
    crashed: List[str]
    failed: List[str]
    start: int
    end: int
    elapsed: int


_JEPSEN_TIME_FORMAT = "%Y-%m-%d %H:%M:%S"
_JEPSEN_MILLI_RE = re.compile("([0-9]+){(.*)}")
_JEPSEN_TIME_RE = re.compile("[0-9]{4}-[0-8]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2},[0-9]+{.*}")


def _time_parse(time: str):
    split = time.split(",")
    date = datetime.strptime(split[0], _JEPSEN_TIME_FORMAT)
    match = _JEPSEN_MILLI_RE.match(split[1])
    microseconds = 0
    if match:
        microseconds = int(match[1]) * 1000

    return date.replace(microsecond=microseconds, tzinfo=timezone.utc)


def _calc_time_from_log(log: str) -> Tuple[int, int, int]:
    if not log:
        return (0, 0, 0)
    start_time = None
    end_time = None
    for line in log.splitlines():
        if _JEPSEN_TIME_RE.match(line):
            if start_time is None:
                start_time = _time_parse(line)
            else:
                end_time = _time_parse(line)

    if start_time is None or end_time is None:
        return (0, 0, 0)

    elapsed_time = int(end_time.timestamp() - start_time.timestamp())

    return (int(start_time.timestamp()), int(end_time.timestamp()), elapsed_time)


SUCCESS_RE = re.compile("([0-9]+) successes")
CRASH_RE = re.compile("([0-9]+) crashed")
UNKNOWN_RE = re.compile("([0-9]+) unknown")
FAIL_RE = re.compile("([0-9]+) failures")


def parse(text: List[str]) -> ParserOutput:  # noqa: D406
    """Given a List of strings representing jepsen log file split by newlines, return the ParserOutput struct.

    Args:
        text (List[str]): List of strings representing the jepsen log file split by newlines.

    Returns:
        ParserOutput: The parsed output struct.

    Raises:
        AssertionError: If there is a mismatch between the count of matches and the corresponding test list.
    """

    successful_tests: List[str] = []
    indeterminate_tests: List[str] = []
    crashed_tests: List[str] = []
    failed_tests: List[str] = []
    target = None
    success_table_matches = 0
    unknown_table_matches = 0
    crash_table_matches = 0
    fail_table_matches = 0

    # we're parsing this kind of table through entire file:

    # # Successful tests
    # store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20230718T215050.000Z
    # 1 successes
    # 0 unknown
    # 0 crashed
    # 0 failures

    # # Crashed tests
    # store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20230718T005314.000Z
    # 0 successes
    # 0 unknown
    # 1 crashed
    # 0 failures

    # # Failed tests tests
    # store/mongodb list-append w:majority r:majority tw:majority tr:snapshot partition/20230718T220100.000Z
    # 0 successes
    # 0 unknown
    # 0 crashed
    # 1 failures

    for line in text:
        if "# Successful tests" in line:
            target = successful_tests
            continue
        elif "# Indeterminate tests" in line:
            target = indeterminate_tests
            continue
        elif "# Crashed tests" in line:
            target = crashed_tests
            continue
        elif "# Failed tests" in line:
            target = failed_tests
            continue

        s_match = SUCCESS_RE.match(line)
        if s_match:
            target = None
            success_table_matches += int(s_match[1])

        u_match = UNKNOWN_RE.match(line)
        if u_match:
            target = None
            unknown_table_matches += int(u_match[1])
        c_match = CRASH_RE.match(line)
        if c_match:
            target = None
            crash_table_matches += int(c_match[1])
        f_match = FAIL_RE.match(line)
        if f_match:
            target = None
            fail_table_matches += int(f_match[1])
        if target is not None and line.strip():
            target.append(line)

    assert success_table_matches == len(
        successful_tests
    ), "Mismatch in success_table_matches and length of successful_tests"
    assert unknown_table_matches == len(
        indeterminate_tests
    ), "Mismatch in unknown_table_matches and length of indeterminate_tests"
    assert crash_table_matches == len(
        crashed_tests
    ), "Mismatch in crash_table_matches and length of crashed_tests"
    assert fail_table_matches == len(
        failed_tests
    ), "Mismatch in fail_table_matches and length of failed_tests"

    return ParserOutput(
        {
            "success": successful_tests,
            "unknown": indeterminate_tests,
            "crashed": crashed_tests,
            "failed": failed_tests,
        }
    )


def _try_find_log_file(store: Optional[str], test_name) -> str:
    if store is None:
        return ""

    try:
        with open(os.path.join(store, test_name, "jepsen.log")) as fh:
            return fh.read()

    except Exception:  # pylint: disable=broad-except
        return ""


def report(
    out: ParserOutput, start_time: int, end_time: int, elapsed: int, store: Optional[str]
) -> Report:
    """Given ParserOutput, return report.json as a dict."""

    results = []
    failures = 0
    for test_name in out["success"]:
        log_raw = _try_find_log_file(store, test_name)
        start_time, end_time, elapsed_time = _calc_time_from_log(log_raw)
        results.append(
            Result(
                status="pass",
                exit_code=0,
                test_file=test_name,
                start=start_time,
                end=end_time,
                elapsed=elapsed_time,
                log_raw=log_raw,
            )
        )

    for test_name in out["failed"]:
        log_raw = _try_find_log_file(store, test_name)
        start_time, end_time, elapsed_time = _calc_time_from_log(log_raw)
        failures += 1
        results.append(
            Result(
                status="fail",
                exit_code=1,
                test_file=test_name,
                start=start_time,
                end=end_time,
                elapsed=elapsed_time,
                log_raw=log_raw,
            )
        )

    for test_name in out["crashed"]:
        log_raw = "Log files are unavailable for crashed tests because Jepsen does not save them separately. You may be able to find the exception and stack trace in the task log"
        failures += 1
        results.append(
            Result(
                status="fail",
                exit_code=1,
                test_file=test_name,
                start=start_time,
                end=end_time,
                elapsed=elapsed,
                log_raw=log_raw,
            )
        )

    for test_name in out["unknown"]:
        log_raw = _try_find_log_file(store, test_name)
        start_time, end_time, elapsed_time = _calc_time_from_log(log_raw)
        failures += 1
        results.append(
            Result(
                status="fail",
                exit_code=1,
                test_file=test_name,
                start=start_time,
                end=end_time,
                elapsed=elapsed_time,
                log_raw=log_raw,
            )
        )
    return Report(
        {
            "failures": failures,
            "results": results,
        }
    )


def _get_log_lines(filename: str) -> List[str]:
    with open(filename) as fh:
        return fh.read().splitlines()


def _put_report(report_: Report) -> None:
    with open("report.json", "w") as fh:
        json.dump(report_, fh)


@click.command()
@click.option("--start_time", type=int, required=True)
@click.option("--end_time", type=int, required=True)
@click.option("--elapsed", type=int, required=True)
@click.option(
    "--emit_status_files",
    type=bool,
    is_flag=True,
    default=False,
    help="If true, emit status files for marking Evergreen tasks as system fails",
)
@click.option(
    "--store", type=str, default=None, help="Path to folder containing jepsen 'store' directory"
)
@click.argument("filename", type=str)
def main(
    filename: str,
    start_time: str,
    end_time: str,
    elapsed: str,
    emit_status_files: bool,
    store: Optional[str],
):
    """Generate Evergreen reports from the Jepsen list-append workload."""

    out = parse(_get_log_lines(filename))
    _put_report(report(out, start_time, end_time, elapsed, store))

    exit_code = 255
    if out["crashed"]:
        exit_code = 2
        if emit_status_files:
            with open("jepsen_system_fail.txt", "w") as fh:
                fh.write(str(exit_code))
    else:
        if out["unknown"] or out["failed"]:
            exit_code = 1
        else:
            exit_code = 0

    sys.exit(exit_code)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
