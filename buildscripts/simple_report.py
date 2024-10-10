"""Given a test name, path to log file and exit code, generate/append an Evergreen report.json."""

import json
import os
import pathlib
from typing import List

import click
from typing_extensions import TypedDict


class Result(TypedDict, total=False):
    """Evergreen test result."""

    status: str
    exit_code: int
    test_file: str
    start: float
    end: float
    elapsed: float
    log_raw: str


class Report(TypedDict):
    """Evergreen report."""

    failures: int
    results: List[Result]


def _open_and_truncate_log_lines(log_file: pathlib.Path) -> List[str]:
    with open(log_file) as fh:
        lines = fh.read().splitlines()
        for i, line in enumerate(lines):
            if line == "scons: done reading SConscript files.":
                offset = i
                # if possible, also shave off the current and next line
                # as they contain:
                # scons: done reading SConscript files.
                # scons: Building targets ...
                # which is superfluous.
                if len(lines) > i + 2:
                    offset = i + 2
                return lines[offset:]

        return lines


def _clean_log_file(log_file: pathlib.Path, dedup_lines: bool) -> str:
    lines = _open_and_truncate_log_lines(log_file)
    if dedup_lines:
        lines = _dedup_lines(lines)
    return os.linesep.join(lines)


def make_report(test_name: str, log_file_contents: str, exit_code: int) -> Report:
    status = "pass" if exit_code == 0 else "fail"
    return Report(
        {
            "failures": 0 if exit_code == 0 else 1,
            "results": [
                Result(
                    {
                        "status": status,
                        "exit_code": exit_code,
                        "test_file": test_name,
                        "log_raw": log_file_contents,
                    }
                )
            ],
        }
    )


def try_combine_reports(out: Report):
    try:
        with open("report.json") as fh:
            report = json.load(fh)
            out["results"] += report["results"]
            out["failures"] += report["failures"]
    except NameError:
        pass
    except IOError:
        pass


def _dedup_lines(lines: List[str]) -> List[str]:
    return list(set(lines))


def put_report(out: Report):
    with open("report.json", "w") as fh:
        json.dump(out, fh)


@click.command()
@click.option("--test-name", required=True, type=str)
@click.option("--log-file", required=True, type=pathlib.Path)
@click.option("--exit-code", required=True, type=int)
@click.option("--dedup-lines", is_flag=True)
def main(test_name: str, log_file: pathlib.Path, exit_code: int, dedup_lines: bool):
    """Given a test name, path to log file and exit code, generate/append an Evergreen report.json."""
    log_file_contents = _clean_log_file(log_file, dedup_lines)
    report = make_report(test_name, log_file_contents, exit_code)
    try_combine_reports(report)
    put_report(report)


if __name__ == "__main__":
    main()  # pylint: disable=no-value-for-parameter
