"""Tools to dump debug info for each OS."""

import itertools
import logging
import os
import sys
import tempfile
from abc import ABCMeta, abstractmethod
from collections import namedtuple
from distutils import spawn

from buildscripts.resmokelib.hang_analyzer.process import call, callo, find_program
from buildscripts.resmokelib.hang_analyzer.process_list import Pinfo

Dumpers = namedtuple('Dumpers', ['dbg', 'jstack'])


def get_dumpers(root_logger: logging.Logger, dbg_output: str):
    """
    Return OS-appropriate dumpers.

    :param root_logger: Top-level logger
    :param dbg_output: 'stdout' or 'file'
    """

    dbg = None
    jstack = None
    if sys.platform.startswith("linux"):
        dbg = GDBDumper(root_logger, dbg_output)
        jstack = JstackDumper()
    elif sys.platform == "win32" or sys.platform == "cygwin":
        dbg = WindowsDumper(root_logger, dbg_output)
        jstack = JstackWindowsDumper()
    elif sys.platform == "darwin":
        dbg = LLDBDumper(root_logger, dbg_output)
        jstack = JstackDumper()

    return Dumpers(dbg=dbg, jstack=jstack)


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

    @abstractmethod
    def dump_info(
            self,
            pinfo: Pinfo,
            take_dump: bool,
    ):
        """
        Perform dump for a process.

        :param pinfo: A Pinfo describing the process
        :param take_dump: Whether to take a core dump
        """
        raise NotImplementedError("dump_info must be implemented in OS-specific subclasses")

    @abstractmethod
    def get_dump_ext(self):
        """Return the dump file extension."""
        raise NotImplementedError("get_dump_ext must be implemented in OS-specific subclasses")

    @abstractmethod
    def _find_debugger(self, debugger):
        """
        Find the installed debugger.

        :param debugger: debugger executable.
        """
        raise NotImplementedError("_find_debugger must be implemented in OS-specific subclasses")

    @abstractmethod
    def _prefix(self):
        """Return the commands to set up a debugger process."""
        raise NotImplementedError("_prefix must be implemented in OS-specific subclasses")

    @abstractmethod
    def _process_specific(self, pinfo: Pinfo, take_dump: bool, logger: logging.Logger = None):
        """
        Return the commands that attach to each process, dump info and detach.

        :param pinfo: A Pinfo describing the process
        :param take_dump: Whether to take a core dump
        :param logger: Logger to output dump info to
        """
        raise NotImplementedError("_process_specific must be implemented in OS-specific subclasses")

    @abstractmethod
    def _postfix(self):
        """Return the commands to exit the debugger."""
        raise NotImplementedError("_postfix must be implemented in OS-specific subclasses")


class WindowsDumper(Dumper):
    """WindowsDumper class."""

    def _find_debugger(self, debugger):
        """Find the installed debugger."""
        # We are looking for c:\Program Files (x86)\Windows Kits\8.1\Debuggers\x64
        cdb = spawn.find_executable(debugger)
        if cdb is not None:
            return cdb
        from win32com.shell import shell, shellcon  # pylint: disable=import-outside-toplevel

        # Cygwin via sshd does not expose the normal environment variables
        # Use the shell api to get the variable instead
        root_dir = shell.SHGetFolderPath(0, shellcon.CSIDL_PROGRAM_FILESX86, None, 0)

        # Construct the debugger search paths in most-recent order
        debugger_paths = [os.path.join(root_dir, "Windows Kits", "10", "Debuggers", "x64")]
        for idx in reversed(range(0, 2)):
            debugger_paths.append(
                os.path.join(root_dir, "Windows Kits", "8." + str(idx), "Debuggers", "x64"))

        for dbg_path in debugger_paths:
            self._root_logger.info("Checking for debugger in %s", dbg_path)
            if os.path.exists(dbg_path):
                return os.path.join(dbg_path, debugger)

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
            dump_file = "dump_%s.%d.%s" % (os.path.splitext(pinfo.name)[0], pinfo.pidv,
                                           self.get_dump_ext())
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

    def dump_info(self, pinfo, take_dump):
        """Dump useful information to the console."""
        debugger = "cdb.exe"
        dbg = self._find_debugger(debugger)

        if dbg is None:
            self._root_logger.warning("Debugger %s not found, skipping dumping of %s", debugger,
                                      str(pinfo.pidv))
            return

        self._root_logger.info("Debugger %s, analyzing %s processes with PIDs %s", dbg, pinfo.name,
                               str(pinfo.pidv))

        for pid in pinfo.pidv:
            logger = _get_process_logger(self._dbg_output, pinfo.name, pid=pid)

            process = Pinfo(name=pinfo.name, pidv=pid)
            cmds = self._prefix() + self._process_specific(process, take_dump) + self._postfix()

            call([dbg, '-c', ";".join(cmds), '-p', str(pid)], logger)

            self._root_logger.info("Done analyzing %s process with PID %d", pinfo.name, pid)

    def get_dump_ext(self):
        """Return the dump file extension."""
        return "mdmp"


# LLDB dumper is for MacOS X
class LLDBDumper(Dumper):
    """LLDBDumper class."""

    @staticmethod
    def _find_debugger(debugger):
        """Find the installed debugger."""
        return find_program(debugger, ['/usr/bin'])

    def _prefix(self):
        pass

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

    def dump_info(self, pinfo, take_dump):
        """Dump info."""
        debugger = "lldb"
        dbg = self._find_debugger(debugger)
        logger = _get_process_logger(self._dbg_output, pinfo.name)

        if dbg is None:
            self._root_logger.warning("Debugger %s not found, skipping dumping of %s", debugger,
                                      str(pinfo.pidv))
            return

        self._root_logger.info("Debugger %s, analyzing %s processes with PIDs %s", dbg, pinfo.name,
                               str(pinfo.pidv))

        lldb_version = callo([dbg, "--version"], logger)

        logger.info(lldb_version)

        # Do we have the XCode or LLVM version of lldb?
        # Old versions of lldb do not work well when taking commands via a file
        # XCode (7.2): lldb-340.4.119
        # LLVM - lldb version 3.7.0 ( revision )

        if 'version' not in lldb_version:
            # We have XCode's lldb
            lldb_version = lldb_version[lldb_version.index("lldb-"):]
            lldb_version = lldb_version.replace('lldb-', '')
            lldb_major_version = int(lldb_version[:lldb_version.index('.')])
            if lldb_major_version < 340:
                logger.warning("Debugger lldb is too old, please upgrade to XCode 7.2")
                return

        cmds = self._process_specific(pinfo, take_dump) + self._postfix()

        tf = tempfile.NamedTemporaryFile(mode='w', encoding='utf-8')

        for cmd in cmds:
            tf.write(cmd + "\n")

        tf.flush()

        # Works on in MacOS 10.9 & later
        #call([dbg] +  list( itertools.chain.from_iterable([['-o', b] for b in cmds])), logger)
        call(['cat', tf.name], logger)
        call([dbg, '--source', tf.name], logger)

        self._root_logger.info("Done analyzing %s processes with PIDs %s", pinfo.name,
                               str(pinfo.pidv))

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


# GDB dumper is for Linux
class GDBDumper(Dumper):
    """GDBDumper class."""

    def _find_debugger(self, debugger):
        """Find the installed debugger."""
        return find_program(debugger, ['/opt/mongodbtoolchain/v3/bin', '/usr/bin'])

    def _prefix(self):
        """Return the commands to set up a debugger process."""
        script_dir = "buildscripts"
        gdb_dir = os.path.join(script_dir, "gdb")
        mongo_script = os.path.join(gdb_dir, "mongo.py")
        mongo_printers_script = os.path.join(gdb_dir, "mongo_printers.py")
        mongo_lock_script = os.path.join(gdb_dir, "mongo_lock.py")

        add_venv_sys_path = f"py sys.path.extend({sys.path})"  # Makes venv packages available in GDB
        source_mongo = "source %s" % mongo_script
        source_mongo_printers = "source %s" % mongo_printers_script
        source_mongo_lock = "source %s" % mongo_lock_script

        cmds = [
            "set interactive-mode off",
            "set print thread-events off",  # Suppress GDB messages of threads starting/finishing.
            "set python print-stack full",
            add_venv_sys_path,
            source_mongo,
            source_mongo_printers,
            source_mongo_lock,
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
            mongodb_dump_mutexes = "mongodb-dump-mutexes"
            mongodb_dump_recovery_units = "mongodb-dump-recovery-units"
            mongodb_dump_storage_engine_info = "mongodb-dump-storage-engine-info"

            for pid in pinfo.pidv:
                if not logger.mongo_process_filename:
                    set_logging_on_commands = []
                    set_logging_off_commands = []
                    raw_stacks_commands = []
                else:
                    base, ext = os.path.splitext(logger.mongo_process_filename)
                    set_logging_on_commands = [
                        'set logging file %s_%d%s' % (base, pid, ext), 'set logging on'
                    ]
                    set_logging_off_commands = ['set logging off']
                    raw_stacks_filename = "%s_%d_raw_stacks%s" % (base, pid, ext)
                    raw_stacks_commands = [
                        'echo \\nWriting raw stacks to %s.\\n' % raw_stacks_filename,
                        # This sends output to log file rather than stdout until we turn logging off.
                        'set logging redirect on',
                        'set logging file ' + raw_stacks_filename,
                        'set logging on',
                        'thread apply all bt',
                        'set logging off',
                        'set logging redirect off',
                    ]

                mongodb_waitsfor_graph = "mongodb-waitsfor-graph debugger_waitsfor_%s_%d.gv" % \
                    (pinfo.name, pid)

                cmds += set_logging_on_commands + [
                    "attach %d" % pid,
                    "handle SIGSTOP ignore noprint",
                    "info sharedlibrary",
                    "info threads",  # Dump a simple list of commands to get the thread name
                ] + set_logging_off_commands + raw_stacks_commands + set_logging_on_commands + [
                    mongodb_uniqstack,
                    # Lock the scheduler, before running commands, which execute code in the attached process.
                    "set scheduler-locking on",
                    mongodb_dump_locks,
                    mongodb_show_locks,
                    mongodb_waitsfor_graph,
                    mongodb_javascript_stack,
                    mongod_dump_sessions,
                    mongodb_dump_mutexes,
                    mongodb_dump_recovery_units,
                    mongodb_dump_storage_engine_info,
                    "detach",
                ] + set_logging_off_commands

        return cmds

    def _postfix(self):
        """Return the commands to exit the debugger."""
        cmds = ["set confirm off", "quit"]
        return cmds

    def dump_info(self, pinfo, take_dump):
        """Dump info."""
        debugger = "gdb"
        dbg = self._find_debugger(debugger)
        logger = _get_process_logger(self._dbg_output, pinfo.name)

        if dbg is None:
            self._root_logger.warning("Debugger %s not found, skipping dumping of %s", debugger,
                                      str(pinfo.pidv))
            return

        self._root_logger.info("Debugger %s, analyzing %s processes with PIDs %s", dbg, pinfo.name,
                               str(pinfo.pidv))

        call([dbg, "--version"], logger)

        cmds = self._prefix() + self._process_specific(pinfo, take_dump, logger) + self._postfix()

        call([dbg, "--quiet", "--nx"] + list(
            itertools.chain.from_iterable([['-ex', b] for b in cmds])), logger)

        self._root_logger.info("Done analyzing %s processes with PIDs %s", pinfo.name,
                               str(pinfo.pidv))

    def get_dump_ext(self):
        """Return the dump file extension."""
        return "core"

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
    def _find_debugger(debugger):
        """Find the installed jstack debugger."""
        return find_program(debugger, ['/usr/bin'])

    def dump_info(self, root_logger, dbg_output, pid, process_name):
        """Dump java thread stack traces to the console."""
        debugger = "jstack"
        jstack = self._find_debugger(debugger)
        logger = _get_process_logger(dbg_output, process_name, pid=pid)

        if jstack is None:
            logger.warning("Debugger %s not found, skipping dumping of %d", debugger, pid)
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

    if 'stdout' in dbg_output:
        s_handler = logging.StreamHandler(sys.stdout)
        s_handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        process_logger.addHandler(s_handler)

    if 'file' in dbg_output:
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

    def __init__(self, dump_pids, message=("Failed to create core dumps for some processes,"
                                           " SIGABRT will be sent as a fallback if -k is set.")):
        """Initialize error."""
        self.dump_pids = dump_pids
        self.message = message
        super().__init__(self.message)
