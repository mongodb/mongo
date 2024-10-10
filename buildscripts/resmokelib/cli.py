"""Command-line entry-point into resmoke."""

import os
import time
from datetime import datetime

import psutil
from mongo_tooling_metrics.lib.top_level_metrics import ResmokeToolingMetrics

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
    subcommand = parser.parse_command_line(
        argv[1:],
        start_time=__start_time,
        usage="Resmoke is MongoDB's correctness testing orchestrator.\n"
        "For more information, see the help message for each subcommand.\n"
        "For example: resmoke.py run -h\n"
        "Note: bisect, setup-multiversion and symbolize subcommands have been moved to db-contrib-tool (https://github.com/10gen/db-contrib-tool#readme).\n",
    )
    ResmokeToolingMetrics.register_metrics(
        utc_starttime=datetime.utcfromtimestamp(__start_time),
        parser=parser.get_parser(),
    )
    subcommand.execute()
