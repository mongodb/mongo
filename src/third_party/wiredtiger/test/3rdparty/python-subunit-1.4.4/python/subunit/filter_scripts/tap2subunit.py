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

"""A filter that reads a TAP stream and outputs a subunit stream.

More information on TAP is available at
http://testanything.org/wiki/index.php/Main_Page.
"""

import sys

from subunit import TAP2SubUnit


def main():
    sys.exit(TAP2SubUnit(sys.stdin, sys.stdout))


if __name__ == '__main__':
    main()
