import argparse

from buildscripts.resmokelib.hang_analyzer.hang_analyzer import HangAnalyzer
from buildscripts.resmokelib.plugin import PluginInterface


class HangAnalyzerPlugin(PluginInterface):
    """Integration-point for hang-analyzer."""

    def parse(
        self,
        subcommand: str,
        parser: argparse.ArgumentParser,
        parsed_args: dict,
        should_configure_otel: bool = True,
        **kwargs,
    ):
        """Parse command-line options."""
        if subcommand == "hang-analyzer":
            return HangAnalyzer(parsed_args, task_id=parsed_args["task_id"], **kwargs)
        return None

    def add_subcommand(self, subparsers):
        """Create and add the parser for the hang analyzer subcommand."""
        parser = subparsers.add_parser("hang-analyzer", help=__doc__)

        parser.add_argument(
            "-m",
            "--process-match",
            dest="process_match",
            choices=("contains", "exact"),
            default="contains",
            help="Type of match for process names (-p & -g), specify 'contains', or"
            " 'exact'. Note that the process name match performs the following"
            " conversions: change all process names to lowercase, strip off the file"
            " extension, like '.exe' on Windows. Default is 'contains'.",
        )
        parser.add_argument(
            "-p",
            "--process-names",
            dest="process_names",
            help="Comma separated list of process names to analyze",
        )
        parser.add_argument(
            "-g",
            "--go-process-names",
            dest="go_process_names",
            help="Comma separated list of go process names to analyze",
        )
        parser.add_argument(
            "-d",
            "--process-ids",
            dest="process_ids",
            default=None,
            help="Comma separated list of process ids (PID) to analyze, overrides -p & -g",
        )
        parser.add_argument(
            "-c",
            "--dump-core",
            dest="dump_core",
            action="store_true",
            default=False,
            help="Dump core file for each analyzed process",
        )
        parser.add_argument(
            "-s",
            "--max-disk-usage-percent",
            dest="max_disk_usage_percent",
            default=90,
            help="Maximum disk usage percent for a core dump",
        )
        parser.add_argument(
            "-o",
            "--debugger-output",
            dest="debugger_output",
            action="append",
            choices=("file", "stdout"),
            default=None,
            help="If 'stdout', then the debugger's output is written to the Python"
            " process's stdout. If 'file', then the debugger's output is written"
            " to a file named debugger_<process>_<pid>.log for each process it"
            " attaches to. This option can be specified multiple times on the"
            " command line to have the debugger's output written to multiple"
            " locations. By default, the debugger's output is written only to the"
            " Python process's stdout.",
        )
        parser.add_argument(
            "-k",
            "--kill-processes",
            dest="kill_processes",
            action="store_true",
            default=False,
            help="Kills the analyzed processes after analysis completes.",
        )
        parser.add_argument(
            "--task-id",
            "-t",
            action="store",
            type=str,
            default=None,
            help="Fetch corresponding symbols given an Evergreen task ID",
        )
