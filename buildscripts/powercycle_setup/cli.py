"""Command-line entry-point for powercycle_operations."""

from buildscripts.powercycle_setup import parser


def main(argv):
    """
    Execute Main function for powercycle_operations.

    :param argv: sys.argv
    :return: None
    """
    subcommand = parser.parse_command_line(argv[1:])
    subcommand.execute()
