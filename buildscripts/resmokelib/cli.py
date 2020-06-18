"""Command-line entry-point into resmoke."""

import time

from buildscripts.resmokelib import parser


def main(argv):
    """
    Execute Main function for resmoke.

    :param argv: sys.argv
    :return: None
    """
    __start_time = time.time()
    subcommand = parser.parse_command_line(argv[1:], start_time=__start_time)
    subcommand.execute()
