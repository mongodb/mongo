"""Hang Analyzer module.

A prototype hang analyzer for Evergreen integration to help investigate test timeouts

1. Script supports taking dumps, and/or dumping a summary of useful information about a process
2. Script will iterate through a list of interesting processes,
    and run the tools from step 1. The list of processes can be provided as an option.
3. Java processes will be dumped using jstack, if available.

Supports Linux, MacOS X, and Windows.
"""

import getpass
import logging
import os
import platform
import re
import signal
import sys
import time
import traceback

import distro
import psutil

from buildscripts.resmokelib import config as resmoke_config
from buildscripts.resmokelib.hang_analyzer import dumper, process, process_list
from buildscripts.resmokelib.hang_analyzer.extractor import download_debug_symbols
from buildscripts.resmokelib.plugin import PluginInterface, Subcommand
from buildscripts.resmokelib.symbolizer import Symbolizer


class HangAnalyzer(Subcommand):
    """Main class for the hang analyzer subcommand."""

    def __init__(self, options, task_id=None, logger=None, **_kwargs):
        """
        Configure processe lists based on options.

        :param options: Options as parsed by parser.py
        :param logger: Logger to be used. If not specified, one will be created.
        :param kwargs: additional args
        """
        self.options = options
        self.root_logger = None
        self.interesting_processes = [
            # Remove "python", "java" from the list to avoid hang analyzer multiple invocations
            "mongo",
            "mongod",
            "mongos",
            "_test",
            "dbtest",
        ]
        self.go_processes = []
        self.process_ids = []

        def configure_task_id():
            run_tid = resmoke_config.EVERGREEN_TASK_ID
            hang_analyzer_tid = task_id
            if run_tid and hang_analyzer_tid and run_tid != hang_analyzer_tid:
                raise ValueError(
                    "The Evergreen Task ID (tid) should be either passed in through `resmoke.py run` "
                    "or through `resmoke.py hang-analyzer` but not both. run tid: %s, hang-analyzer tid: %s"
                    % (run_tid, hang_analyzer_tid)
                )
            return run_tid or hang_analyzer_tid

        self.task_id = configure_task_id()
        self._configure_processes()
        self._setup_logging(logger)

    def kill_rogue_processes(self):
        """Kill any processes that are currently being analyzed."""
        processes = process_list.get_processes(
            self.process_ids,
            self.interesting_processes,
            self.options.process_match,
            self.root_logger,
        )
        process.teardown_processes(self.root_logger, processes, dump_pids={})

    def execute(self):
        """
        Execute hang analysis.

        1. Get a list of interesting processes
        2. Dump useful information or take core dumps
        """

        self._log_system_info()

        dumpers = dumper.get_dumpers(self.root_logger, self.options.debugger_output)

        processes = process_list.get_processes(
            self.process_ids,
            self.interesting_processes,
            self.options.process_match,
            self.root_logger,
        )

        def is_python_process(pname: str):
            # "live-record*" and "python*" are Python processes. Sending SIGUSR1 causes resmoke.py
            # to dump its stack and run the hang analyzer on its child processes.
            # Sending SIGUSR1 causes live-record to save its recording and terminate.
            return pname.startswith("python") or pname.startswith("live-record")

        # Suspending all processes, except python, to prevent them from getting unstuck when
        # the hang analyzer attaches to them.
        for pinfo in [pinfo for pinfo in processes if not is_python_process(pinfo.name)]:
            for pid in pinfo.pidv:
                process.pause_process(self.root_logger, pinfo.name, pid)

        # Dump python processes by signalling them. The resmoke.py process will generate
        # the report.json, when signalled, so we do this before attaching to other processes.
        for pinfo in [pinfo for pinfo in processes if is_python_process(pinfo.name)]:
            for pid in pinfo.pidv:
                process.signal_python(self.root_logger, pinfo.name, pid)

        trapped_exceptions = []

        dump_pids = {}
        # Dump core files of all processes, except python & java.
        if self.options.dump_core:
            take_core_processes = [
                pinfo for pinfo in processes if not re.match("^(java|python)", pinfo.name)
            ]
            if os.getenv("ASAN_OPTIONS") or os.getenv("TSAN_OPTIONS"):
                quit_processes: list[psutil.Process] = []
                for pinfo in take_core_processes:
                    for pid in pinfo.pidv:
                        # The mongo signal processing thread needs to be resumed to handle the SIGABRT.
                        quit_process = psutil.Process(pid)
                        process.resume_process(self.root_logger, pinfo.name, pid)
                        self.root_logger.info(
                            "Process %d may be running a sanitizer which uses a large amount of virtual memory.",
                            pid,
                        )
                        self.root_logger.info(
                            "Attempting to send SIGABRT from resmoke to capture a more manageable sized core dump"
                        )
                        process.signal_process(self.root_logger, pid, signal.SIGABRT)
                        quit_processes.append(quit_process)
                self.root_logger.info("Waiting for all processes to end after SIGABRT")
                assert isinstance(dumpers.dbg, dumper.GDBDumper)
                timeout = dumpers.dbg.get_timeout_secs()
                start_time = time.time()
                # Wait until all processes successfully end
                while True:
                    alive_processes = []
                    # This loop filters out processes that have ended or become a zombie
                    for quit_process in quit_processes:
                        if (
                            quit_process.is_running()
                            and quit_process.status() != psutil.STATUS_ZOMBIE
                        ):
                            alive_processes.append(quit_process)

                    # Update the quit_processes list with only the ones left alive
                    quit_processes = alive_processes

                    # All the processes have ended
                    if not alive_processes:
                        break

                    if time.time() - start_time > timeout:
                        raise RuntimeError(
                            f"The following processes took too long to end after SIGABRT: {alive_processes}"
                        )

                    time.sleep(0.1)
                self.root_logger.info("Finished waiting for all processes to end.")
            else:
                for pinfo in take_core_processes:
                    if self._check_enough_free_space():
                        try:
                            dumpers.dbg.dump_info(pinfo, take_dump=True)
                        except dumper.DumpError as err:
                            self.root_logger.error(err.message)
                            dump_pids = {**err.dump_pids, **dump_pids}
                        except Exception as err:  # pylint: disable=broad-except
                            self.root_logger.info(
                                "Error encountered when invoking debugger %s", err
                            )

                            trapped_exceptions.append(traceback.format_exc())
                    else:
                        self.root_logger.info(
                            "Not enough space for a core dump, skipping %s processes with PIDs %s",
                            pinfo.name,
                            str(pinfo.pidv),
                        )

        # Download symbols after pausing if the task ID is not None and not running with sanitizers.
        # Sanitizer builds are not stripped and don't require debug symbols.
        san_options = os.environ.get("san_options", None)
        if self.task_id is not None and san_options is None:
            my_symbolizer = Symbolizer(self.task_id, download_symbols_only=True)
            download_debug_symbols(self.root_logger, my_symbolizer)

        # Dump info of all processes, except python & java.
        for pinfo in [pinfo for pinfo in processes if not re.match("^(java|python)", pinfo.name)]:
            try:
                dumpers.dbg.dump_info(pinfo, take_dump=False)
            except Exception as err:  # pylint: disable=broad-except
                self.root_logger.info("Error encountered when invoking debugger %s", err)
                trapped_exceptions.append(traceback.format_exc())

        # Dump java processes using jstack.
        for pinfo in [pinfo for pinfo in processes if pinfo.name.startswith("java")]:
            for pid in pinfo.pidv:
                try:
                    dumpers.jstack.dump_info(
                        self.root_logger, self.options.debugger_output, pinfo.name, pid
                    )
                except Exception as err:  # pylint: disable=broad-except
                    self.root_logger.info("Error encountered when invoking debugger %s", err)
                    trapped_exceptions.append(traceback.format_exc())

        # Signal go processes to ensure they print out stack traces, and die on POSIX OSes.
        # On Windows, this will simply kill the process since python emulates SIGABRT as
        # TerminateProcess.
        # Note: The stacktrace output may be captured elsewhere (i.e. resmoke).
        for pinfo in [pinfo for pinfo in processes if pinfo.name in self.go_processes]:
            for pid in pinfo.pidv:
                self.root_logger.info(
                    "Sending signal SIGABRT to go process %s with PID %d", pinfo.name, pid
                )
                process.signal_process(self.root_logger, pid, signal.SIGABRT)

        self.root_logger.info("Done analyzing all processes for hangs")

        # Kill and abort processes if "-k" was specified.
        if self.options.kill_processes:
            process.teardown_processes(self.root_logger, processes, dump_pids)
        else:
            # Resuming all suspended processes.
            for pinfo in [pinfo for pinfo in processes if not pinfo.name.startswith("python")]:
                for pid in pinfo.pidv:
                    process.resume_process(self.root_logger, pinfo.name, pid)

        for exception in trapped_exceptions:
            self.root_logger.info(exception)
        if trapped_exceptions:
            raise RuntimeError(
                "Exceptions were thrown while dumping. There may still be some valid dumps."
            )

    def _configure_processes(self):
        if self.options.debugger_output is None:
            self.options.debugger_output = ["stdout"]

        if self.options.process_ids is not None:
            # self.process_ids is an int list of PIDs
            self.process_ids = [int(pid) for pid in self.options.process_ids.split(",")]

        if self.options.process_names is not None:
            self.interesting_processes = self.options.process_names.split(",")

        if self.options.go_process_names is not None:
            self.go_processes = self.options.go_process_names.split(",")
            self.interesting_processes += self.go_processes

    def _setup_logging(self, logger):
        if logger is None:
            self.root_logger = logging.Logger("hang_analyzer", level=logging.DEBUG)
            handler = logging.StreamHandler(sys.stdout)
            handler.setFormatter(logging.Formatter(fmt="%(message)s"))
            self.root_logger.addHandler(handler)
        else:
            self.root_logger = logger

    def _log_system_info(self):
        self.root_logger.info("Python Version: %s", sys.version)
        self.root_logger.info("OS: %s", platform.platform())

        try:
            if sys.platform in ["win32", "cygwin"]:
                self.root_logger.info("Windows Distribution: %s", platform.win32_ver())
            else:
                self.root_logger.info("Linux Distribution: %s", distro.linux_distribution())

        except AttributeError:
            self.root_logger.warning("Cannot determine Linux distro since Python is too old")

        try:
            current_login = getpass.getuser()
            self.root_logger.info("Current Login: %s", current_login)
            uid = os.getuid()
            self.root_logger.info("Current UID: %s", uid)
        except AttributeError:
            self.root_logger.warning(
                "Cannot determine Unix Current Login, not supported on Windows"
            )

    def _check_enough_free_space(self):
        usage_percent = psutil.disk_usage(".").percent
        self.root_logger.info("Current disk usage percent: %s", usage_percent)
        return usage_percent < self.options.max_disk_usage_percent


class HangAnalyzerPlugin(PluginInterface):
    """Integration-point for hang-analyzer."""

    def parse(self, subcommand, parser, parsed_args, **kwargs):
        """Parse command-line options."""
        if subcommand == "hang-analyzer":
            return HangAnalyzer(parsed_args, task_id=parsed_args.task_id, **kwargs)
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
            " conversions: change all process names to lowecase, strip off the file"
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
            help="Comma separated list of process ids (PID) to analyze, overrides -p &" " -g",
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
