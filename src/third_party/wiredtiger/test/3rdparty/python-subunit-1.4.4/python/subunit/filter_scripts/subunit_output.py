#!/usr/bin/env python3
#  subunit: extensions to python unittest to get test results from subprocesses.
#  Copyright (C) 2013 Subunit Contributors
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


"""A command-line tool to generate a subunit result byte-stream."""

import sys

from subunit._output import output_main


def main():
    sys.exit(output_main())


if __name__ == '__main__':
    main()
