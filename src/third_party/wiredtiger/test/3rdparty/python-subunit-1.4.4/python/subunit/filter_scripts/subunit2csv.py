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
#  distributed under these licenses is d on an "AS IS" BASIS, WITHOUT
#  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
#  license you chose for the specific language governing permissions and
#  limitations under that license.
#

"""Turn a subunit stream into a CSV"""

from testtools import StreamToExtendedDecorator

from subunit.filters import run_filter_script
from subunit.test_results import CsvResult


def main():
    run_filter_script(lambda output: StreamToExtendedDecorator(
        CsvResult(output)), __doc__, protocol_version=2)


if __name__ == '__main__':
    main()
