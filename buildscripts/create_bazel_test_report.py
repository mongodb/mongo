import json
import time
import xml.etree.ElementTree as ET
from glob import glob
from typing import List

import typer
from typing_extensions import TypedDict
from util.expansions import get_expansion


class Result(TypedDict):
    """A single test result"""

    status: str
    test_file: str
    log_raw: str
    start: float
    end: float


class Report(TypedDict):
    """Test report for Evergreen"""

    results: List[Result]


def main(testlog_dir: str):
    """Create an report.json for Evergreen from bazel test logs."""

    if not get_expansion("create_bazel_test_report"):
        print(
            "Expansion create_bazel_test_report is not set, skipping creating a report from Bazel test logs."
        )
        return

    report = Report({"results": []})
    for test_xml in glob(f"{testlog_dir}/**/test.xml", recursive=True):
        testsuite = ET.parse(test_xml).getroot().find("testsuite")
        testcase = testsuite.find("testcase")
        test_file = testcase.attrib["name"]

        if testcase.find("error") is not None:
            status = "fail"
        else:
            status = "pass"

        log_raw = testsuite.find("system-out").text

        # Bazel gives just a duration, while Evergreen expects a start and end
        # time to calculate the duration. Evergreen itself does something similar
        # for when only a duration is known.
        duration = testcase.attrib["time"]
        start = time.time()
        end = start + int(duration)

        report["results"].append(
            Result(
                {
                    "test_file": test_file,
                    "status": status,
                    "start": start,
                    "end": end,
                    "log_raw": log_raw,
                }
            )
        )

    if report["results"]:
        with open("report.json", "wt") as fh:
            json.dump(report, fh)
    else:
        print(f"No test.xml found within {testlog_dir}. Not creating a report.")


if __name__ == "__main__":
    typer.run(main)
