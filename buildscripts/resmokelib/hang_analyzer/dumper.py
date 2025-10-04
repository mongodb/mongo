"""Tools to dump debug info for each OS."""

import glob
import itertools
import logging
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from abc import ABCMeta, abstractmethod
from collections import namedtuple
from datetime import datetime, timedelta
from distutils import spawn
from io import StringIO
from typing import List, Tuple

import psutil
from opentelemetry import trace
from opentelemetry.trace.status import StatusCode

from buildscripts.resmokelib import config as resmoke_config
from buildscripts.resmokelib.hang_analyzer.process import (
    call,
    callo,
    find_program,
    resume_process,
    signal_process,
)
from buildscripts.resmokelib.hang_analyzer.process_list import Pinfo
from buildscripts.resmokelib.utils.otel_utils import get_default_current_span
from buildscripts.simple_report import Report, Result

Dumpers = namedtuple("Dumpers", ["dbg", "jstack"])
TRACER = trace.get_tracer("resmoke")


def get_dumpers(root_logger: logging.Logger, dbg_output: str, include_terminating=False):
    """
    Return OS-appropriate dumpers.

    :param root_logger: Top-level logger
    :param dbg_output: 'stdout' or 'file'
    :param include_terminating: If dumpers that terminate processes should be included.
    """

    dbg = None
    jstack = None
    if sys.platform.startswith("linux"):
        dbg = GDBDumper(root_logger, dbg_output)
        jstack = JstackDumper()
    elif sys.platform in ["win32", "cygwin"]:
        dbg = WindowsDumper(root_logger, dbg_output)
        jstack = JstackWindowsDumper()
    elif sys.platform == "darwin":
        dbg = LLDBDumper(root_logger, dbg_output)
        jstack = JstackDumper()

    if include_terminating:
        if not dbg.find_debugger():
            root_logger.info(
                "No debugger found. Will send SIGABRT to terminate processes to allow the OS to generate a core dump."
            )
            dbg = SigabrtDumper(root_logger, dbg_output)
        elif os.getenv("ASAN_OPTIONS") or os.getenv("TSAN_OPTIONS"):
            root_logger.info(
                "ASAN_OPTIONS or TSAN_OPTIONS is set. Will send SIGABRT rather than attaching with a debugger to avoid high memory usage and bloated core dump size."
            )
            dbg = SigabrtDumper(root_logger, dbg_output)

    return Dumpers(dbg=dbg, jstack=jstack)


def find_files(file_name: str, path: str) -> List[str]:
    return glob.glob(f"{path}/**/{file_name}", recursive=True)


class Dumper(metaclass=ABCMeta):
    """
    Abstract base class for OS-specific dumpers.

    :param dbg_output: 'stdout' or 'file'
    :param root_logger: Top-level logger
    """

    def __init__(self, root_logger: logging.Logger, dbg_output: str):
        """Initialize dumper."""
        self._root_logger = root_logger
        self._dbg_output = dbg_output

        if resmoke_config.EVERGREEN_TASK_ID is None:
            # Set 24 hours time out for hang analyzer being run in locally
            self._time_limit = timedelta(days=1)
        else:
            # Timeout for dumping is 12mins (out of total 15mins for the entire hang-analyzer) in Evergreen
            self._time_limit = timedelta(minutes=12)

    def dump_info(self, pinfo: Pinfo, take_dump: bool):
        """
        Perform dump for a process.

        :param pinfo: A Pinfo describing the process
        :param take_dump: Whether to take a core dump
        """
        if self._time_limit <= timedelta(0):
            self._root_logger.warning(
                "Skipping dumping of %s processes with PIDs %s because the time limit expired.",
                pinfo.name,
                str(pinfo.pidv),
            )
            return

        start_time = datetime.now()

        self._dump_impl(pinfo, take_dump)

        elapsed = datetime.now() - start_time
        self._time_limit -= elapsed

    @abstractmethod
    def _dump_impl(self, pinfo: Pinfo, take_dump: bool):
        """
        Perform dump for a process.

        :param pinfo: A Pinfo describing the process
        :param take_dump: Whether to take a core dump
        """
        raise NotImplementedError("_dump_impl must be implemented in OS-specific subclasses")

    @abstractmethod
    def find_debugger(self):
        """Find the installed debugger."""
        raise NotImplementedError("find_debugger must be implemented in OS-specific subclasses")


class SigabrtDumper(Dumper):
    """
    A platform independent dumper that simply sends SIGABRT to the process, allowing
    the OS the opportunity to generate a core dump. Note this will almost always
    terminate the process.
    """

    def _dump_impl(self, pinfo: Pinfo, take_dump: bool):
        """
        Perform dump for a process.

        :param pinfo: A Pinfo describing the process
        :param take_dump: Whether to take a core dump
        """

        if take_dump:
            processes = []
            for pid in pinfo.pidv:
                processes.append(psutil.Process(pid))
                self._root_logger.info(f"Sending SIGABRT to {pid}")
                signal_process(self._root_logger, pid, signal.SIGABRT)
                resume_process(self._root_logger, pinfo.name, pid)

            self.wait_for_processes(processes)

    def wait_for_processes(self, processes):
        self._root_logger.info("Waiting for processes to end after SIGABRT")

        start_time = datetime.now()
        alive = []
        while datetime.now() - start_time < self._time_limit:
            alive = []
            # This loop filters out processes that have ended or become a zombie
            for p in processes:
                if p.is_running() and p.status() != psutil.STATUS_ZOMBIE:
                    alive.append(p)

            # Update the processes list with only the ones left alive
            processes = alive

            # All the processes have ended
            if not alive:
                return

            time.sleep(1)

        self._root_logger.warning(
            f"The following processes took too long to end after SIGABRT: {alive}"
        )

    def find_debugger(self):
        return None


class WindowsDumper(Dumper):
    """WindowsDumper class."""

    def find_debugger(self):
        """Find the installed debugger."""
        # We are looking for c:\Program Files (x86)\Windows Kits\8.1\Debuggers\x64
        debugger = "cdb.exe"
        cdb = spawn.find_executable(debugger)
        if cdb is not None:
            return cdb
        from win32com.shell import shell, shellcon

        # Cygwin via sshd does not expose the normal environment variables
        # Use the shell api to get the variable instead
        root_dir = shell.SHGetFolderPath(0, shellcon.CSIDL_PROGRAM_FILESX86, None, 0)

        # Construct the debugger search paths in most-recent order
        debugger_paths = [os.path.join(root_dir, "Windows Kits", "10", "Debuggers", "x64")]
        for idx in reversed(range(0, 2)):
            debugger_paths.append(
                os.path.join(root_dir, "Windows Kits", "8." + str(idx), "Debuggers", "x64")
            )

        for dbg_path in debugger_paths:
            self._root_logger.info("Checking for debugger in %s", dbg_path)
            cdb = os.path.join(dbg_path, debugger)
            if os.path.exists(cdb):
                return cdb

        return None

    def _prefix(self):
        """Return the commands to set up a debugger process."""
        cmds = [
            ".symfix",  # Fixup symbol path
            "!sym noisy",  # Enable noisy symbol loading
            ".symopt +0x10",  # Enable line loading (off by default in CDB, on by default in WinDBG)
            ".reload",  # Reload symbols
        ]

        return cmds

    def _process_specific(self, pinfo, take_dump, logger=None):
        """Return the commands that attach to each process, dump info and detach."""
        assert isinstance(pinfo.pidv, int)

        if take_dump:
            # Dump to file, dump_<process name>.<pid>.mdmp
            dump_file = "dump_%s.%d.%s" % (
                os.path.splitext(pinfo.name)[0],
                pinfo.pidv,
                self.get_dump_ext(),
            )
            dump_command = ".dump /ma %s" % dump_file
            self._root_logger.info("Dumping core to %s", dump_file)

            cmds = [
                dump_command,
                ".detach",  # Detach
            ]
        else:
            cmds = [
                "!peb",  # Dump current exe, & environment variables
                "lm",  # Dump loaded modules
                "!uniqstack -pn",  # Dump All unique Threads with function arguments
                "!cs -l",  # Dump all locked critical sections
                ".detach",  # Detach
            ]

        return cmds

    def _postfix(self):
        """Return the commands to exit the debugger."""
        cmds = [
            "q"  # Quit
        ]

        return cmds

    def _dump_impl(self, pinfo, take_dump):
        """Dump useful information to the console."""
        dbg = self.find_debugger()

        if dbg is None:
            self._root_logger.warning("Debugger not found, skipping dumping of %s", str(pinfo.pidv))
            return

        self._root_logger.info(
            "Debugger %s, analyzing %s processes with PIDs %s", dbg, pinfo.name, str(pinfo.pidv)
        )

        for pid in pinfo.pidv:
            logger = _get_process_logger(self._dbg_output, pinfo.name, pid=pid)

            process = Pinfo(name=pinfo.name, pidv=pid)
            cmds = self._prefix() + self._process_specific(process, take_dump) + self._postfix()

            call(
                [dbg, "-c", ";".join(cmds), "-p", str(pid)],
                logger,
                timeout_seconds=self._time_limit.total_seconds(),
            )

            self._root_logger.info("Done analyzing %s process with PID %d", pinfo.name, pid)

    def analyze_cores(self, core_file_dir: str, install_dir: str, analysis_dir: str):
        install_dir = os.path.abspath(install_dir)
        core_files = find_files(f"*.{self.get_dump_ext()}", core_file_dir)
        if not core_files:
            raise RuntimeError(f"No core dumps found in {core_file_dir}")
        for filename in core_files:
            file_path = os.path.abspath(filename)
            try:
                self.analyze_core(file_path, install_dir)
            except Exception as e:
                self._root_logger.error("Failed to process core dump: %s", filename)
                self._root_logger.error(e)

    def analyze_core(self, core_file_path: str, install_dir: str):
        filename = os.path.basename(core_file_path)
        regex = re.search(rf"dump_(.+)\.([0-9]+)\.{self.get_dump_ext()}", filename)

        if not regex:
            self._root_logger.warning(
                "Core dump file name does not match expected pattern, skipping %s", filename
            )
            return

        binary_name = f"{regex.group(1)}.exe"
        pid = int(regex.group(2))
        binary_files = find_files(binary_name, install_dir)
        logger = _get_process_logger(self._dbg_output, binary_name, pid)
        logger.info("analyzing %s", filename)

        if not binary_files:
            logger.warn("Binary %s not found, cannot process %s", binary_name, filename)
            return

        if len(binary_files) > 1:
            logger.error("More than one file found in %s matching %s", install_dir, binary_name)
            raise RuntimeError(f"More than one file found in {install_dir} matching {binary_name}")

        binary_path = binary_files[0]
        symbol_path = binary_path.replace(".exe", ".pdb")

        dbg = self.find_debugger()

        if dbg is None:
            self._root_logger.warning("Debugger not found, skipping dumping of %s", filename)
            return

        cmds = (
            self._prefix()
            + [
                "!peb",  # Dump current exe, & environment variables
                "lm",  # Dump loaded modules
                "!uniqstack -pn",  # Dump All unique Threads with function arguments
                "!cs -l",  # Dump all locked critical sections
            ]
            + self._postfix()
        )

        call(
            [dbg, "-i", binary_path, "-z", core_file_path, "-y", symbol_path, "-v", ";".join(cmds)],
            logger,
        )

    def get_dump_ext(self):
        """Return the dump file extension."""
        return "mdmp"


# LLDB dumper is for MacOS X
class LLDBDumper(Dumper):
    """LLDBDumper class."""

    @staticmethod
    def find_debugger():
        """Find the installed debugger."""
        debugger = "lldb"
        return find_program(debugger, ["/usr/bin"])

    def _process_specific(self, pinfo, take_dump, logger=None):
        """Return the commands that attach to each process, dump info and detach."""
        cmds = []

        if take_dump:
            dump_files = self._dump_files(pinfo)
            for pid in pinfo.pidv:
                # Dump to file, dump_<process name>.<pid>.core
                dump_file = dump_files[pid]
                dump_command = "process save-core %s" % dump_file
                self._root_logger.info("Dumping core to %s", dump_file)

                cmds += [
                    "platform shell kill -CONT %d" % pid,
                    "attach -p %d" % pid,
                    dump_command,
                    "process detach",
                    "platform shell kill -STOP %d" % pid,
                ]
        else:
            for pid in pinfo.pidv:
                cmds += [
                    "platform shell kill -CONT %d" % pid,
                    "attach -p %d" % pid,
                    "target modules list",
                    "thread backtrace all",
                    "process detach",
                    "platform shell kill -STOP %d" % pid,
                ]

        return cmds

    def _postfix(self):
        """Return the commands to exit the debugger."""
        cmds = [
            "settings set interpreter.prompt-on-quit false",
            "quit",
        ]

        return cmds

    def _dump_impl(self, pinfo, take_dump):
        """Dump info."""
        dbg = self.find_debugger()
        logger = _get_process_logger(self._dbg_output, pinfo.name)

        if dbg is None:
            self._root_logger.warning("Debugger not found, skipping dumping of %s", str(pinfo.pidv))
            return

        self._root_logger.info(
            "Debugger %s, analyzing %s processes with PIDs %s", dbg, pinfo.name, str(pinfo.pidv)
        )

        lldb_version = callo([dbg, "--version"], logger)

        logger.info(lldb_version)

        # Do we have the XCode or LLVM version of lldb?
        # Old versions of lldb do not work well when taking commands via a file
        # XCode (7.2): lldb-340.4.119
        # LLVM - lldb version 3.7.0 ( revision )

        if "version" not in lldb_version:
            # We have XCode's lldb
            lldb_version = lldb_version[lldb_version.index("lldb-") :]
            lldb_version = lldb_version.replace("lldb-", "")
            lldb_major_version = int(lldb_version[: lldb_version.index(".")])
            if lldb_major_version < 340:
                logger.warning("Debugger lldb is too old, please upgrade to XCode 7.2")
                return

        cmds = self._process_specific(pinfo, take_dump) + self._postfix()

        tf = tempfile.NamedTemporaryFile(mode="w", encoding="utf-8")

        for cmd in cmds:
            tf.write(cmd + "\n")

        tf.flush()

        # Works on in MacOS 10.9 & later
        # call([dbg] +  list( itertools.chain.from_iterable([['-o', b] for b in cmds])), logger)
        call(["cat", tf.name], logger)
        call([dbg, "--source", tf.name], logger, timeout_seconds=self._time_limit.total_seconds())

        self._root_logger.info(
            "Done analyzing %s processes with PIDs %s", pinfo.name, str(pinfo.pidv)
        )

        if take_dump:
            need_sigabrt = {}
            files = self._dump_files(pinfo)
            for pid in files:
                if not os.path.exists(files[pid]):
                    need_sigabrt[pid] = files[pid]
            if need_sigabrt:
                raise DumpError(need_sigabrt)

    def get_dump_ext(self):
        """Return the dump file extension."""
        return "core"

    def _dump_files(self, pinfo):
        """Return a dict mapping pids to core dump filenames that this dumper can create."""
        files = {}
        for pid in pinfo.pidv:
            files[pid] = "dump_%s.%d.%s" % (pinfo.name, pid, self.get_dump_ext())
        return files

    def get_binary_from_core_dump(self, core_file_path):
        raise NotImplementedError("get_binary_from_core_dump is not implemented on macos")


# GDB dumper is for Linux
class GDBDumper(Dumper):
    """GDBDumper class."""

    def find_debugger(self):
        """Find the installed debugger."""
        debugger = "gdb"
        return find_program(
            debugger, ["/opt/mongodbtoolchain/v5/bin", "/opt/mongodbtoolchain/v4/bin", "/usr/bin"]
        )

    def _prefix(self):
        """Return the commands to set up a debugger process."""

        add_venv_sys_path = (
            f"py sys.path.extend({sys.path})"  # Makes venv packages available in GDB
        )

        cmds = [
            "set interactive-mode off",
            "set print thread-events off",  # Suppress GDB messages of threads starting/finishing.
            add_venv_sys_path,
            "source .gdbinit",
        ]
        return cmds

    def _process_specific(self, pinfo, take_dump, logger=None):
        """Return the commands that attach to each process, dump info and detach."""
        cmds = []

        if take_dump:
            for pid in pinfo.pidv:
                # Dump to file, dump_<process name>.<pid>.core
                dump_file = "dump_%s.%d.%s" % (pinfo.name, pid, self.get_dump_ext())
                dump_command = "gcore %s" % dump_file
                self._root_logger.info("Dumping core to %s", dump_file)
                cmds += [
                    "attach %d" % pid,
                    "handle SIGSTOP ignore noprint",
                    # Lock the scheduler, before running commands, which execute code in the attached process.
                    "set scheduler-locking on",
                    dump_command,
                    "detach",
                ]
        else:
            mongodb_dump_locks = "mongodb-dump-locks"
            mongodb_show_locks = "mongodb-show-locks"
            mongodb_uniqstack = "mongodb-uniqstack mongodb-bt-if-active"
            mongodb_javascript_stack = "mongodb-javascript-stack"
            mongod_dump_sessions = "mongod-dump-sessions"
            mongodb_dump_recovery_units = "mongodb-dump-recovery-units"
            mongodb_dump_storage_engine_info = "mongodb-dump-storage-engine-info"

            for pid in pinfo.pidv:
                filename = "debugger_%s_%d.log" % (os.path.splitext(pinfo.name)[0], pid)
                if logger:
                    base, ext = os.path.splitext(filename)
                    set_logging_on_commands = [
                        "set logging file %s_%d%s" % (base, pid, ext),
                        "set logging on",
                    ]
                    set_logging_off_commands = ["set logging off"]
                    raw_stacks_filename = "%s_%d_raw_stacks%s" % (base, pid, ext)
                    raw_stacks_commands = [
                        "echo \\nWriting raw stacks to %s.\\n" % raw_stacks_filename,
                        # This sends output to log file rather than stdout until we turn logging off.
                        "set logging redirect on",
                        "set logging file " + raw_stacks_filename,
                        "set logging on",
                        "thread apply all bt",
                        "set logging off",
                        "set logging redirect off",
                    ]
                else:
                    set_logging_on_commands = []
                    set_logging_off_commands = []
                    raw_stacks_commands = []

                mongodb_waitsfor_graph = "mongodb-waitsfor-graph debugger_waitsfor_%s_%d.gv" % (
                    pinfo.name,
                    pid,
                )

                cmds += (
                    set_logging_on_commands
                    + [
                        "attach %d" % pid,
                        "handle SIGSTOP ignore noprint",
                        "info sharedlibrary",
                        "info threads",  # Dump a simple list of commands to get the thread name
                    ]
                    + set_logging_off_commands
                    + raw_stacks_commands
                    + set_logging_on_commands
                    + [
                        mongodb_uniqstack,
                        # Lock the scheduler, before running commands, which execute code in the attached process.
                        "set scheduler-locking on",
                        mongodb_dump_locks,
                        mongodb_show_locks,
                        mongodb_waitsfor_graph,
                        mongodb_javascript_stack,
                        mongod_dump_sessions,
                        mongodb_dump_recovery_units,
                        mongodb_dump_storage_engine_info,
                        "detach",
                    ]
                    + set_logging_off_commands
                )

        return cmds

    def _postfix(self):
        """Return the commands to exit the debugger."""
        cmds = ["set confirm off", "quit"]
        return cmds

    def _dump_impl(self, pinfo, take_dump):
        """Dump info."""

        dbg = self.find_debugger()
        logger = self._root_logger

        if dbg is None:
            self._root_logger.warning("Debugger not found, skipping dumping of %s", str(pinfo.pidv))
            return

        self._root_logger.info(
            "Debugger %s, analyzing %s processes with PIDs %s", dbg, pinfo.name, str(pinfo.pidv)
        )

        call([dbg, "--version"], logger)

        cmds = self._prefix() + self._process_specific(pinfo, take_dump, logger) + self._postfix()

        # gcore is both a command within GDB and a script packaged alongside gdb. The gcore script
        # invokes the gdb binary with --readnever to avoid spending time loading the debug symbols
        # prior to taking the core dump. The debug symbols are unneeded to generate the core dump.
        #
        # For reference:
        # https://sourceware.org/git/?p=binutils-gdb.git;a=blob;f=gdb/gcore.in;h=34860de630cf0ee766e102eb82f7a3fddba6b368#l101
        skip_reading_symbols_on_take_dump = ["--readnever"] if take_dump else []

        # Live process debugging is not done in the core analyzer subcommand and is no longer
        # supported in the hang analyzer. The only gdb commands we run here are to take core
        # dumps of the running processes.
        if take_dump:
            call(
                [dbg, "--quiet", "--nx"]
                + skip_reading_symbols_on_take_dump
                + list(itertools.chain.from_iterable([["-ex", b] for b in cmds])),
                logger,
                self._time_limit.total_seconds(),
                pinfo,
            )

        self._root_logger.info(
            "Done analyzing %s processes with PIDs %s", pinfo.name, str(pinfo.pidv)
        )

    @TRACER.start_as_current_span("core_analyzer.analyze_cores")
    def analyze_cores(
        self,
        core_file_dir: str,
        install_dir: str,
        analysis_dir: str,
        multiversion_dir: str,
        gdb_index_cache: str,
    ) -> Report:
        core_files = find_files(f"*.{self.get_dump_ext()}", core_file_dir)
        analyze_cores_span = get_default_current_span()
        if not core_files:
            analyze_cores_span.set_status(StatusCode.ERROR, "No core dumps found")
            analyze_cores_span.set_attribute(
                "analyze_cores_error", f"No core dumps found in {core_file_dir}"
            )
            raise RuntimeError(f"No core dumps found in {core_file_dir}")

        tmp_dir = os.path.join(analysis_dir, "tmp")
        os.makedirs(tmp_dir, exist_ok=True)
        report = Report({"failures": 0, "results": []})
        for filename in core_files:
            core_file_path = os.path.abspath(filename)
            basename = os.path.basename(filename)
            self._root_logger.info("Starting analysis of %s", basename)
            log_stream = StringIO()
            logger = logging.Logger(basename, level=logging.DEBUG)
            handler = logging.StreamHandler(log_stream)
            handler.setFormatter(logging.Formatter(fmt="%(message)s"))
            logger.addHandler(handler)
            with TRACER.start_as_current_span("core_analyzer.analyze_core") as analyze_core_span:
                start_time = time.time()
                analyze_core_span.set_status(StatusCode.OK)
                try:
                    exit_code, status = self.analyze_core(
                        core_file_path=core_file_path,
                        install_dir=install_dir,
                        analysis_dir=analysis_dir,
                        tmp_dir=tmp_dir,
                        logger=logger,
                        multiversion_dir=multiversion_dir,
                        gdb_index_cache=gdb_index_cache,
                    )
                except Exception:
                    logger.exception("Exception occured while analyzing core")
                    exit_code = 1
                    status = "fail"

                analyze_core_span.set_attributes(
                    {
                        "analyze_core_status": status,
                        "core_file": core_file_path,
                    }
                )
                if status == "fail":
                    analyze_core_span.set_status(
                        StatusCode.ERROR, description="Failed to analyze core dump."
                    )
            end_time = time.time()
            output = log_stream.getvalue()
            result = Result(
                Result(
                    {
                        "status": status,
                        "exit_code": exit_code,
                        "test_file": basename,
                        "log_raw": output,
                        "start": start_time,
                        "end": end_time,
                        "elapsed": end_time - start_time,
                    }
                )
            )
            if exit_code == 1:
                report["failures"] += 1
            report["results"].append(result)
            self._root_logger.info("Analysis of %s ended with status %s", basename, status)
        analyze_cores_span.set_attributes(
            {"failures": report["failures"], "core_dump_count": len(core_files)}
        )
        shutil.rmtree(tmp_dir)
        return report

    def analyze_core(
        self,
        core_file_path: str,
        install_dir: str,
        analysis_dir: str,
        tmp_dir: str,
        multiversion_dir: str,
        logger: logging.Logger,
        gdb_index_cache: str,
    ) -> Tuple[int, str]:  # returns (exit_code, test_status)
        cmds = []
        dbg = self.find_debugger()
        readelf = find_program("eu-readelf", ["/opt/mongodbtoolchain/v5/bin", "/usr/bin"])
        basename = os.path.basename(core_file_path)
        if dbg is None:
            self._root_logger.error("Debugger not found, skipping dumping of %s", basename)
            return 1, "fail"

        # ensure debugger version is loggged
        call([dbg, "--version"], logger)
        call([readelf, "--version"], logger, check=False)
        lib_dir = None

        binary_name, bin_version = self.get_binary_from_core_dump(core_file_path)
        search_dir = multiversion_dir if bin_version else install_dir
        binary_files = find_files(binary_name, search_dir)

        if not binary_files:
            # This can sometimes happen because coredumps can appear from non-mongo processes
            logger.warn("Binary %s not found, cannot process %s", binary_name, basename)
            return 0, "skip"

        if len(binary_files) > 1:
            logger.error("More than one file found in %s matching %s", install_dir, binary_name)
            return 1, "fail"

        binary_path = os.path.realpath(os.path.abspath(binary_files[0]))
        lib_dir = os.path.abspath(os.path.join(os.path.dirname(binary_files[0]), "..", "lib"))

        basename = os.path.basename(core_file_path)
        logging_dir = os.path.join(analysis_dir, basename)
        os.makedirs(logging_dir, exist_ok=True)

        cmds += [
            f"set solib-search-path {lib_dir}",
            f"set index-cache directory {tmp_dir}",
            f"set index-cache enabled {gdb_index_cache}",
            f"file {binary_path}",
        ]

        # dump the gnu debuglink just to make sure it exists
        call([readelf, "--string-dump=.gnu_debuglink", binary_path], logger, check=False)

        # We run gdb once to populate the gdb index cache because gdb can sometimes
        # behave weirdly and take a long time when pulling from the index cache
        # in the same invocation it creates the index cache.
        gdb_index_cmds = self._prefix() + cmds + self._postfix()
        args = [dbg, "--nx"] + list(
            itertools.chain.from_iterable([["-ex", b] for b in gdb_index_cmds])
        )
        exit_code = call(args, logger, check=False)

        if exit_code != 0:
            raise Exception("Bad exit code %d from %s" % (exit_code, " ".join(args)))

        cmds += [
            f"core-file {core_file_path}",
            "python import gdbmongo",
            "python gdbmongo.register_printers()",
            "set width 0",
        ]

        def add_commands(command: str, name: str):
            file_path = os.path.join(logging_dir, f"{basename}.{name}.txt")
            cmds.extend(
                [
                    f"echo \\nWriting {name} to {file_path}.\\n",
                    f"set logging file {file_path}",
                    "set logging enabled on",
                    command,
                    "set logging enabled off",
                ]
            )

        add_commands("info threads", "info_threads")
        add_commands("thread apply all bt", "backtraces")
        add_commands("mongodb-uniqstack mongodb-bt-if-active", "uniqstack")
        add_commands("mongodb-show-locks", "show_locks")
        add_commands("mongod-dump-sessions", "dump_sessions")
        add_commands("mongodb-dump-recovery-units", "dump_recovery_units")
        # depends on gdbmongo python dependency
        add_commands("python print(gdbmongo.LockManagerPrinter.from_global().val)", "dump_locks")

        cmds = self._prefix() + cmds + self._postfix()

        args = [dbg, "--nx"] + list(itertools.chain.from_iterable([["-ex", b] for b in cmds]))
        exit_code = call(args, logger, check=False)

        current_span = trace.get_current_span()
        current_span.set_attribute("gdb_exit_code", exit_code)

        if exit_code != 0:
            raise Exception("Bad exit code %d from %s" % (exit_code, " ".join(args)))

        return 0, "pass"

    def get_dump_ext(self):
        """Return the dump file extension."""
        return "core"

    def get_binary_from_core_dump(self, core_file_path):
        dbg = self.find_debugger()
        if dbg is None:
            raise RuntimeError("Debugger not found, can't run get_binary_from_core_dump")
        process = subprocess.run(
            [dbg, "-batch", "--quiet", "-ex", f"core {core_file_path}"],
            check=True,
            capture_output=True,
            text=True,
        )

        regex = re.search("Core was generated by `(.*)'.", process.stdout)
        if not regex:
            raise RuntimeError("gdb output did not match pattern, could not find binary name")

        binary_path = regex.group(1)
        binary_name = binary_path.split(" ")[0]
        binary_name = binary_name.split("/")[-1]
        regex = re.match(r"[a-zA-Z]+-([0-9]{1,2}\.[0-9])", binary_name)
        bin_version = None
        if regex:
            bin_version = regex.group(1)
        return binary_name, bin_version

    @staticmethod
    def _find_gcore():
        """Find the installed gcore."""
        dbg = "/usr/bin/gcore"
        if os.path.exists(dbg):
            return dbg

        return None


# jstack is a JDK utility
class JstackDumper(object):
    """JstackDumper class."""

    @staticmethod
    def find_debugger():
        """Find the installed jstack debugger."""
        debugger = "jstack"
        return find_program(debugger, ["/usr/bin"])

    def dump_info(self, root_logger, dbg_output, pid, process_name):
        """Dump java thread stack traces to the console."""
        jstack = self.find_debugger()
        logger = _get_process_logger(dbg_output, process_name, pid=pid)

        if jstack is None:
            logger.warning("Debugger not found, skipping dumping of %d", pid)
            return

        root_logger.info("Debugger %s, analyzing %s process with PID %d", jstack, process_name, pid)

        call([jstack, "-l", str(pid)], logger)

        root_logger.info("Done analyzing %s process with PID %d", process_name, pid)


# jstack is a JDK utility
class JstackWindowsDumper(object):
    """JstackWindowsDumper class."""

    @staticmethod
    def dump_info(root_logger, pid):
        """Dump java thread stack traces to the logger."""

        root_logger.warning("Debugger jstack not supported, skipping dumping of %d", pid)


def _get_process_logger(dbg_output, pname: str, pid: int = None):
    """Return the process logger from options specified."""
    process_logger = logging.Logger("process", level=logging.DEBUG)
    process_logger.mongo_process_filename = None

    if "stdout" in dbg_output:
        s_handler = logging.StreamHandler(sys.stdout)
        s_handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        process_logger.addHandler(s_handler)

    if "file" in dbg_output:
        if pid:
            filename = "debugger_%s_%d.log" % (os.path.splitext(pname)[0], pid)
        else:
            filename = "debugger_%s.log" % (os.path.splitext(pname)[0])
        process_logger.mongo_process_filename = filename
        f_handler = logging.FileHandler(filename=filename, mode="w")
        f_handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        process_logger.addHandler(f_handler)

    return process_logger


class DumpError(Exception):
    """
    Exception raised for errors while dumping processes.

    Tracks what cores still need to be generated.
    """

    def __init__(
        self,
        dump_pids,
        message=(
            "Failed to create core dumps for some processes,"
            " SIGABRT will be sent as a fallback if -k is set."
        ),
    ):
        """Initialize error."""
        self.dump_pids = dump_pids
        self.message = message
        super().__init__(self.message)
