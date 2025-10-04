#!/usr/bin/env python3
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2013  Robert Collins <robertc@robertcollins.net>
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

"""Convert a version 1 subunit stream to version 2 stream."""

import sys
from optparse import OptionParser

from testtools import ExtendedToStreamDecorator

from subunit import StreamResultToBytes
from subunit.filters import find_stream, run_tests_from_stream


def make_options(description):
    parser = OptionParser(description=__doc__)
    return parser


def main():
    parser = make_options(__doc__)
    (options, args) = parser.parse_args()
    run_tests_from_stream(find_stream(sys.stdin, args),
        ExtendedToStreamDecorator(StreamResultToBytes(sys.stdout)))
    sys.exit(0)


if __name__ == '__main__':
    main()
