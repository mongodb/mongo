#!/usr/bin/env python3
#
# Copyright (C) 2021-present MongoDB, Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the Server Side Public License, version 1,
# as published by MongoDB, Inc.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# Server Side Public License for more details.
#
# You should have received a copy of the Server Side Public License
# along with this program. If not, see
# <http://www.mongodb.com/licensing/server-side-public-license>.
#
# As a special exception, the copyright holders give permission to link the
# code of portions of this program with the OpenSSL library under certain
# conditions as described in each individual source file and distribute
# linked combinations including the program with the OpenSSL library. You
# must comply with the Server Side Public License in all respects for
# all of the code used other than as permitted herein. If you modify file(s)
# with this exception, you may extend this exception to your version of the
# file(s), but you are not obligated to do so. If you do not wish to do so,
# delete this exception statement from your version. If you delete this
# exception statement from all source files in the program, then also delete
# it in the license file.
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
