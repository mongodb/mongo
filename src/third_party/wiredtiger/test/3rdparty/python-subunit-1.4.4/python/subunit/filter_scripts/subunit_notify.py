#!/usr/bin/env python3
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2010 Jelmer Vernooij <jelmer@samba.org>
#
#  Licensed under either the Apache License, Version 2.0 or the BSD 3-clause
#  license at the users choice. A copy of both licenses are available in the
#  project source as Apache-2.0 and BSD. You may not use this file except in
#  compliance with one of these two licences.
#  
#  Unless required by applicable law or agreed to in writing, software
#  distributed under these licenses is distributed on an "AS IS" BASIS, WITHOUT
#  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
#  license you chose for the specific language governing permissions and
#  limitations under that license.
#

"""Notify the user of a finished test run."""

import gi
import sys

gi.require_version('Gtk', '3.0')
from gi.repository import Notify  # noqa: E402
from testtools import StreamToExtendedDecorator  # noqa: E402

from subunit import TestResultStats  # noqa: E402
from subunit.filters import run_filter_script  # noqa: E402

if not Notify.init("Subunit-notify"):
    sys.exit(1)


def notify_of_result(result):
    result = result.decorated
    if result.failed_tests > 0:
        summary = "Test run failed"
    else:
        summary = "Test run successful"
    body = "Total tests: %d; Passed: %d; Failed: %d" % (
        result.total_tests,
        result.passed_tests,
        result.failed_tests,
    )
    nw = Notify.Notification(summary, body)
    nw.show()


def main():
    run_filter_script(
        lambda output:StreamToExtendedDecorator(TestResultStats(output)),
        __doc__, notify_of_result, protocol_version=2)


if __name__ == '__main__':
    main()
