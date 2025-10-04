#!/usr/bin/env python3
"""Command line utility for executing operations on remote hosts."""

import os.path
import sys

# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from buildscripts.powercycle_setup import cli

cli.main(sys.argv)
