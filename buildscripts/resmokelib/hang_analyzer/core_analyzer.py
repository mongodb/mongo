import argparse
import json
import logging
import os
import sys
from typing import Optional

from opentelemetry import trace
from opentelemetry.trace.status import StatusCode

from buildscripts.resmokelib import configure_resmoke
from buildscripts.resmokelib.hang_analyzer import dumper
from buildscripts.resmokelib.hang_analyzer.extractor import download_task_artifacts
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.utils.otel_utils import get_default_current_span

TRACER = trace.get_tracer("resmoke")


class CoreAnalyzer(Subcommand):
    def __init__(self, options: argparse.Namespace, logger: logging.Logger = None):
        self.options = options
        self.task_id = options.failed_task_id
        self.execution = options.execution
        self.gdb_index_cache = options.gdb_index_cache
        self.root_logger = self.setup_logging(logger)

    @TRACER.start_as_current_span("core_analyzer.execute")
    def execute(self):
        base_dir = self.options.working_dir
        current_span = get_default_current_span({"failed_task_id": self.task_id})
        dumpers = dumper.get_dumpers(self.root_logger, None)

        if self.task_id:
            skip_download = False
            task_id_file = os.path.join(base_dir, "task-id")
            if os.path.exists(task_id_file):
                with open(task_id_file, "r") as file:
                    if file.read().strip() == self.task_id:
                        skip_download = True
                        self.root_logger.info(
                            "Files from task id provided were already on disk, skipping download."
                        )

            multiversion_dir = os.path.join(base_dir, "multiversion")
            if not skip_download and not download_task_artifacts(
                self.root_logger,
                self.task_id,
                base_dir,
                dumpers.dbg,
                multiversion_dir,
                self.execution,
            ):
                self.root_logger.error("Artifacts were not found.")
                current_span.set_attributes(
                    {
                        "core_analyzer_execute_error": "Artifacts were not found.",
                    }
                )
                current_span.set_status(StatusCode.ERROR, description="Artifacts were not found.")
                raise RuntimeError(
                    "Artifacts were not found for specified task. Could not analyze cores."
                )

            with open(task_id_file, "w") as file:
                file.write(self.task_id)

            core_dump_dir = os.path.join(base_dir, "core-dumps")
            install_dir = os.path.join(base_dir, "install")
        else:  # if a task id was not specified, look for input files on the current machine
            install_dir = self.options.install_dir or os.path.join(
                os.path.curdir, "build", "install"
            )
            core_dump_dir = self.options.core_dir or os.path.curdir
            multiversion_dir = self.options.multiversion_dir or os.path.curdir

        analysis_dir = os.path.join(base_dir, "analysis")
        report = dumpers.dbg.analyze_cores(
            core_dump_dir, install_dir, analysis_dir, multiversion_dir, self.gdb_index_cache
        )

        if self.options.generate_report:
            with open("report.json", "w") as file:
                json.dump(report, file)

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

    def parse(
        self,
        subcommand: str,
        parser: argparse.ArgumentParser,
        parsed_args: argparse.Namespace,
        **kwargs,
    ) -> Optional[Subcommand]:
        """Parse command-line options."""
        if subcommand == "core-analyzer":
            configure_resmoke.detect_evergreen_config(parsed_args)
            configure_resmoke.validate_and_update_config(parser, parsed_args)
            return CoreAnalyzer(parsed_args)
        return None

    def add_subcommand(self, subparsers: argparse._SubParsersAction):
        """Create and add the parser for the core analyzer subcommand."""

        parser = subparsers.add_parser(
            "core-analyzer", help="Analyzes the core dumps from the specified input files."
        )

        parser.add_argument(
            "--task-id",
            "-t",
            action="store",
            type=str,
            default=None,
            dest="failed_task_id",
            help="Fetch corresponding core dumps and binaries for a given task id.",
        )

        parser.add_argument(
            "--execution",
            "-e",
            action="store",
            type=int,
            default=None,
            help="The execution of the task you want to download core dumps for."
            " This will default to the latest execution if left blank.",
        )

        parser.add_argument(
            "--install-dir",
            "-b",
            action="store",
            type=str,
            default=None,
            help="Directory that contains binaires and debugsymbols.",
        )

        parser.add_argument(
            "--multiversion-dir",
            "-m",
            action="store",
            type=str,
            default=None,
            help="Directory that contains multiversion binaries and debugsymbols.",
        )

        parser.add_argument(
            "--core-dir",
            "-c",
            action="store",
            type=str,
            default=None,
            help="Directory that contains core dumps.",
        )

        parser.add_argument(
            "--working-dir",
            "-w",
            action="store",
            type=str,
            default="core-analyzer",
            help="Directory that downloaded artifacts will be stored and output will be written to.",
        )

        parser.add_argument(
            "--generate-report",
            "-r",
            action="store_true",
            default=False,
            help="Whether to generate a report used to log individual tests in evergreen.",
        )

        parser.add_argument(
            "--gdb-index-cache",
            "-g",
            action="store",
            default="on",
            choices=["on", "off"],
            metavar="ON|OFF",
            help="Set core analyzer GDB index cache enabled state (default: on)",
        )

        configure_resmoke.add_otel_args(parser)
