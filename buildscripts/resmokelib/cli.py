"""Command-line entry-point into resmoke."""

import os
import time

import psutil

from buildscripts.resmokelib import parser


def main(argv):
    """
    Execute Main function for resmoke.

    :param argv: sys.argv
    :return: None
    """
    __start_time = time.time()
    os.environ["RESMOKE_PARENT_PROCESS"] = str(os.getpid())
    os.environ["RESMOKE_PARENT_CTIME"] = str(psutil.Process().create_time())

    # If invoked by "bazel run", ensure it runs in the workspace root.
    workspace_dir = os.environ.get("BUILD_WORKSPACE_DIRECTORY")
    if workspace_dir:
        os.chdir(workspace_dir)

    subcommand = parser.parse_command_line(
        argv[1:],
        start_time=__start_time,
        usage="Resmoke is MongoDB's correctness testing orchestrator.\n"
        "For more information, see the help message for each subcommand.\n"
        "For example: resmoke.py run -h\n"
        "Note: bisect, setup-multiversion and symbolize subcommands have been moved to db-contrib-tool (https://github.com/10gen/db-contrib-tool#readme).\n",
    )
    subcommand.execute()
