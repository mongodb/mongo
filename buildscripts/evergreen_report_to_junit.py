#!/usr/bin/env python3
"""Convert an Evergreen report.json to JUnit XML.

USAGE
  evergreen_report_to_junit.py --suite <suite> report.json > junit.xml
"""

import argparse
import datetime
import json
import sys
from xml.sax.saxutils import quoteattr


def iso(ts):
    return (
        datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc)
        .isoformat(timespec="milliseconds")
        .replace("+00:00", "Z")
    )


def convert(report, suite):
    out = ['<?xml version="1.0" encoding="UTF-8"?>', "<testsuites>"]
    out += convert_suite(report.get("results", []), suite)
    out.append("</testsuites>")
    return "\n".join(out) + "\n"


def convert_suite(rows, suite):
    indexed = enumerate(rows)
    results = [r for _, r in sorted(indexed, key=lambda p: (p[1].get("start") or 0, p[0]))]
    cases = []
    for r in results:
        name = r.get("test_file") or ""
        cases.append((name, name if "/" in name or "\\" in name else "", r))

    failures = sum(1 for _, _, r in cases if r.get("status") not in ("pass", "skip"))
    skipped = sum(1 for _, _, r in cases if r.get("status") == "skip")
    out = []
    out.append(
        f'  <testsuite name={quoteattr(suite)} tests="{len(cases)}" '
        f'failures="{failures}" skipped="{skipped}">'
    )
    for name, file, r in cases:
        start, end = r.get("start"), r.get("end")
        attrs = f"name={quoteattr(name)} classname={quoteattr(suite)}"
        if file:
            attrs += f" file={quoteattr(file)}"
        if start:
            attrs += f' timestamp="{iso(start)}"'
        if start and end:
            attrs += f' time="{max(0.0, end - start):.3f}"'
        status = r.get("status")
        code = r.get("exit_code")
        display = r.get("display_test_name")
        if status == "pass":
            out.append(f"    <testcase {attrs}/>")
        elif status == "skip":
            out.append(f"    <testcase {attrs}><skipped/></testcase>")
        else:
            # fail, silentfail, null (never finished) are all failures.
            kind = "timeout" if code == -2 else (status or "incomplete")
            # Trunk drops the type attribute, so the message carries the kind.
            msg = f"{kind}: resmoke status={status} exit_code={code}"
            if display:
                msg += f" display_test_name={display}"
            out.append(
                f"    <testcase {attrs}><failure type={quoteattr(kind)} "
                f"message={quoteattr(msg)}/></testcase>"
            )
    out.append("  </testsuite>")
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("report")
    p.add_argument("--suite", required=True, help="suite name; part of the test id")
    a = p.parse_args()
    with open(a.report) as f:
        report = json.load(f)
    sys.stdout.write(convert(report, a.suite))


if __name__ == "__main__":
    main()
