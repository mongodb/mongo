#!/usr/bin/env python3
# Copyright (c) MongoDB, Inc.
# SPDX-License-Identifier: SSPL-1.0
"""Feed command line arguments to a Cheetah template to generate a source file."""

import argparse
import sys
import warnings

warnings.filterwarnings("ignore", message="\nYou don't have the C version of NameMapper installed")

from Cheetah.Template import Template


def main():
    """Generate a source file by passing command line arguments to a Cheetah template.

    The Cheetah template will be expanded with an `$args` in its namespace, containing
    the trailing command line arguments of this program.
    """

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o",
        nargs="?",
        type=argparse.FileType("w"),
        default=sys.stdout,
        help="output file (default sys.stdout)",
    )
    parser.add_argument("template_file", help="Cheetah template file")
    parser.add_argument("template_arg", nargs="*", default=[], help="Cheetah template args")
    opts = parser.parse_args()

    opts.o.write(str(Template(file=opts.template_file, namespaces=[{"args": opts.template_arg}])))


if __name__ == "__main__":
    main()
