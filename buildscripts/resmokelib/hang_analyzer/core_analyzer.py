import argparse
import logging
import os
import sys
from typing import Optional
from buildscripts.resmokelib.hang_analyzer import dumper
from buildscripts.resmokelib.hang_analyzer.extractor import download_task_artifacts
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand


class CoreAnalyzer(Subcommand):
    def __init__(self, options: argparse.Namespace, logger: logging.Logger = None):
        self.options = options
        self.task_id = options.task_id
        self.execution = options.execution
        self.root_logger = self.setup_logging(logger)

    def execute(self):
        base_dir = self.options.working_dir

        if self.task_id:
            skip_download = False
            task_id_file = os.path.join(base_dir, "task-id")
            if os.path.exists(task_id_file):
                with open(task_id_file, "r") as file:
                    if file.read().strip() == self.task_id:
                        skip_download = True
                        self.root_logger.info(
                            "Files from task id provided were already on disk, skipping download.")

            if not skip_download and not download_task_artifacts(self.root_logger, self.task_id,
                                                                 base_dir, self.execution):
                self.root_logger.error("Artifacts were not found.")
                raise RuntimeError(
                    "Artifacts were not found for specified task. Could not analyze cores.")

            with open(task_id_file, "w") as file:
                file.write(self.task_id)

            core_dump_dir = os.path.join(base_dir, "core-dumps")
            install_dir = os.path.join(base_dir, "install")
        else:  # if a task id was not specified, look for input files on the current machine
            install_dir = self.options.install_dir or os.path.join(os.path.curdir, "build",
                                                                   "install")
            core_dump_dir = self.options.core_dir or os.path.curdir

        analysis_dir = os.path.join(base_dir, "analysis")
        dumpers = dumper.get_dumpers(self.root_logger, self.options.debugger_output)
        dumpers.dbg.analyze_cores(core_dump_dir, install_dir, analysis_dir)

    def setup_logging(self, logger: Optional[logging.Logger]):
        if logger is None:
            root_logger = logging.Logger("hang_analyzer", level=logging.DEBUG)
            handler = logging.StreamHandler(sys.stdout)
            handler.setFormatter(logging.Formatter(fmt="%(message)s"))
            root_logger.addHandler(handler)
            return root_logger
        else:
            return logger


class CoreAnalyzerPlugin(PluginInterface):
    """Integration-point for core-analyzer."""

    def parse(self, subcommand: str, parser: argparse.ArgumentParser,
              parsed_args: argparse.Namespace, **kwargs) -> Optional[Subcommand]:
        """Parse command-line options."""
        return CoreAnalyzer(parsed_args) if subcommand == 'core-analyzer' else None

    def add_subcommand(self, subparsers: argparse._SubParsersAction):
        """Create and add the parser for the core analyzer subcommand."""

        parser = subparsers.add_parser(
            "core-analyzer", help="Analyzes the core dumps from the specified input files.")

        parser.add_argument("--task-id", '-t', action="store", type=str, default=None,
                            help="Fetch corresponding core dumps and binaries for a given task id.")

        parser.add_argument(
            "--execution", '-e', action="store", type=int, default=None,
            help="The execution of the task you want to download core dumps for."
            " This will default to the latest execution if left blank.")

        parser.add_argument("--install-dir", '-b', action="store", type=str, default=None,
                            help="Directory that contains binaires and debugsymbols.")

        parser.add_argument("--core-dir", '-c', action="store", type=str, default=None,
                            help="Directory that contains core dumps.")

        parser.add_argument(
            "--working-dir", '-w', action="store", type=str, default="core-analyzer",
            help="Directory that downloaded artifacts will be stores and output will be written to."
        )

        parser.add_argument(
            '-o', '--debugger-output', dest='debugger_output', action="append",
            choices=('file', 'stdout'), default=['stdout'],
            help="If 'stdout', then the debugger's output is written to the Python"
            " process's stdout. If 'file', then the debugger's output is written"
            " to a file named debugger_<process>_<pid>.log for each process it"
            " attaches to. This option can be specified multiple times on the"
            " command line to have the debugger's output written to multiple"
            " locations. By default, the debugger's output is written only to the"
            " Python process's stdout.")
