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
    subcommand = parser.parse_command_line(
        argv[1:], start_time=__start_time,
        usage="Resmoke is MongoDB's correctness testing orchestrator.\n"
        "For more information, see the help message for each subcommand.\n"
        "For example: resmoke.py run -h\n")
    subcommand.execute()
