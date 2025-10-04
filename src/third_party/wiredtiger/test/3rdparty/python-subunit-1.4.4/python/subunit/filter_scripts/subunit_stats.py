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

from subunit import TestResultStats
from subunit.filters import run_filter_script


def main():
    result = TestResultStats(sys.stdout)

    def show_stats(r):
        r.decorated.formatStats()
    
    run_filter_script(
        lambda output:StreamToExtendedDecorator(result),
        __doc__, show_stats, protocol_version=2, passthrough_subunit=False)

if __name__ == '__main__':
    main()
