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
import traceback
from typing import List

import distro
import psutil

from buildscripts.resmokelib import config as resmoke_config
from buildscripts.resmokelib.hang_analyzer import dumper, process, process_list
from buildscripts.resmokelib.plugin import Subcommand


class HangAnalyzer(Subcommand):
    """Main class for the hang analyzer subcommand."""

    def __init__(self, options: dict, task_id=None, logger=None, **_kwargs):
        """
        Configure process lists based on options.

        :param options: Options as parsed by parser.py
        :param logger: Logger to be used. If not specified, one will be created.
        :param kwargs: additional args
        """
        self.options = options
        self.root_logger = None
        self.interesting_processes: List[str] = [
            # Remove "python", "java" from the list to avoid hang analyzer multiple invocations
            "mongo",
            "mongod",
            "mongos",
            "_test",
            "dbtest",
        ]
        self.go_processes: List[str] = []
        self.process_ids: List[int] = []

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
            self.options["process_match"],
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

        dumpers = dumper.get_dumpers(
            self.root_logger,
            self.options["debugger_output"],
            include_terminating=self.options["kill_processes"],
        )

        processes = process_list.get_processes(
            self.process_ids,
            self.interesting_processes,
            self.options["process_match"],
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
        if self.options["dump_core"]:
            take_core_processes = [
                pinfo for pinfo in processes if not re.match("^(java|python)", pinfo.name)
            ]
            for pinfo in take_core_processes:
                if self._check_enough_free_space():
                    try:
                        dumpers.dbg.dump_info(pinfo, take_dump=True)
                    except dumper.DumpError as err:
                        self.root_logger.error(err.message)
                        dump_pids = {**err.dump_pids, **dump_pids}
                    except Exception as err:
                        self.root_logger.info("Error encountered when invoking debugger %s", err)

                        trapped_exceptions.append(traceback.format_exc())
                else:
                    self.root_logger.info(
                        "Not enough space for a core dump, skipping %s processes with PIDs %s",
                        pinfo.name,
                        str(pinfo.pidv),
                    )

        # Dump info of all processes, except python & java.
        for pinfo in [pinfo for pinfo in processes if not re.match("^(java|python)", pinfo.name)]:
            try:
                dumpers.dbg.dump_info(pinfo, take_dump=False)
            except Exception as err:
                self.root_logger.info("Error encountered when invoking debugger %s", err)
                trapped_exceptions.append(traceback.format_exc())

        # Dump java processes using jstack.
        for pinfo in [pinfo for pinfo in processes if pinfo.name.startswith("java")]:
            for pid in pinfo.pidv:
                try:
                    dumpers.jstack.dump_info(
                        self.root_logger, self.options["debugger_output"], pinfo.name, pid
                    )
                except Exception as err:
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
        if self.options["kill_processes"]:
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
        if self.options["debugger_output"] is None:
            self.options["debugger_output"] = ["stdout"]

        # add != "" check to avoid empty process_ids
        if self.options["process_ids"] is not None and self.options["process_ids"] != "":
            # self.process_ids is an int list of PIDs
            self.process_ids = [int(pid) for pid in self.options["process_ids"].split(",")]

        if self.options["process_names"] is not None:
            self.interesting_processes = self.options["process_names"].split(",")

        if self.options["go_process_names"] is not None:
            self.go_processes = self.options["go_process_names"].split(",")
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
                self.root_logger.info(
                    "Linux Distribution: %s %s (%s)",
                    distro.name(),
                    distro.version(),
                    distro.codename(),
                )

        except AttributeError:
            self.root_logger.warning("Cannot determine Linux distro since Python is too old")

        try:
            uid = os.getuid()
            self.root_logger.info("Current UID: %s", uid)
            current_login = getpass.getuser()
            self.root_logger.info("Current Login: %s", current_login)
        except AttributeError:
            self.root_logger.warning(
                "Cannot determine Unix Current Login, not supported on Windows"
            )
        except (KeyError, OSError):
            # The error from getpass.getuser() when there is no username for a UID.
            self.root_logger.warning("No username set for the current UID.")

    def _check_enough_free_space(self):
        usage_percent = psutil.disk_usage(".").percent
        self.root_logger.info("Current disk usage percent: %s", usage_percent)
        return usage_percent < self.options["max_disk_usage_percent"]
