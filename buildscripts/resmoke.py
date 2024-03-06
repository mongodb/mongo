#!/usr/bin/env python3
"""Command line utility for executing MongoDB tests of all kinds."""

import os.path
import sys

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# pylint: disable=wrong-import-position
from buildscripts.resmokelib import cli


# Entrypoint
def entrypoint():
    cli.main(sys.argv)


if __name__ == "__main__":
    entrypoint()
