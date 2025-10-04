#!/usr/bin/env python3
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2009  Robert Collins <robertc@robertcollins.net>
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

"""Filter a subunit stream to get aggregate statistics."""


import sys

from testtools import StreamToExtendedDecorator

from subunit.filters import run_filter_script

try:
    from junitxml import JUnitXmlResult
except ImportError:
    sys.stderr.write("python-junitxml (https://launchpad.net/pyjunitxml or "
        "http://pypi.python.org/pypi/junitxml) is required for this filter.")
    raise


def main():
    run_filter_script(
        lambda output: StreamToExtendedDecorator(
            JUnitXmlResult(output)), __doc__, protocol_version=2)


if __name__ == '__main__':
    main()
