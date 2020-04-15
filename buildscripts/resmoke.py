#!/usr/bin/env python3
"""Command line utility for executing MongoDB tests of all kinds."""

import os.path
import sys
import time

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.resmokelib import parser


def main():
    """Execute Main function for resmoke."""
    __start_time = time.time()
    subcommand = parser.parse_command_line(sys.argv[1:], start_time=__start_time)
    subcommand.execute()


if __name__ == "__main__":
    main()
