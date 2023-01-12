"""Command-line entry-point into resmoke."""

from datetime import datetime
import time
import os
import psutil
from buildscripts.metrics.metrics_datatypes import ResmokeToolingMetrics
from buildscripts.metrics.tooling_exit_hook import initialize_exit_hook
from buildscripts.metrics.tooling_metrics_utils import register_metrics_collection_atexit
from buildscripts.resmokelib import parser


def main(argv):
    """
    Execute Main function for resmoke.

    :param argv: sys.argv
    :return: None
    """
    __start_time = time.time()
    os.environ['RESMOKE_PARENT_PROCESS'] = str(os.getpid())
    os.environ['RESMOKE_PARENT_CTIME'] = str(psutil.Process().create_time())
    subcommand = parser.parse_command_line(
        argv[1:], start_time=__start_time,
        usage="Resmoke is MongoDB's correctness testing orchestrator.\n"
        "For more information, see the help message for each subcommand.\n"
        "For example: resmoke.py run -h\n"
        "Note: bisect and setup-multiversion subcommands have been moved to db-contrib-tool (https://github.com/10gen/db-contrib-tool#readme).\n"
    )
    register_metrics_collection_atexit(ResmokeToolingMetrics.generate_metrics, {
        "utc_starttime": datetime.utcfromtimestamp(__start_time),
        "exit_hook": initialize_exit_hook()
    })
    subcommand.execute()
