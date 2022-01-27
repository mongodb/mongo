#!/usr/bin/env python
#
# Public Domain 2014-present MongoDB, Inc.
# Public Domain 2008-2014 WiredTiger, Inc.
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.

"""Hang Analyzer module.

A prototype hang analyzer for Evergreen integration to help investigate test timeouts.

1. Script supports taking dumps, and/or dumping a summary of useful information about a process.
2. Script will iterate through a list of interesting processes,
    and run the tools from step 1. The list of processes can be provided as an option.

Currently only supports Linux. There are two issues with the MacOS and Windows implementations:
1. WT-6918 - lldb cannot attach to processes in MacOS.
2. WT-6919 - Windows cannot find the debug symbols.
"""

import csv, glob, itertools, logging, re, tempfile, traceback
import os, sys, platform, signal, subprocess, threading, time
from distutils import spawn
from io import BytesIO, TextIOWrapper
from optparse import OptionParser
_IS_WINDOWS = (sys.platform == "win32")

if _IS_WINDOWS:
    import win32event
    import win32api

"""
Helper class to read output of a subprocess.

Used to avoid deadlocks from the pipe buffer filling up and blocking the subprocess while it's
being waited on.
"""
class LoggerPipe(threading.Thread):
    """Asynchronously reads the output of a subprocess and sends it to a logger."""

    # The start() and join() methods are not intended to be called directly on the LoggerPipe
    # instance. Since we override them for that effect, the super's version are preserved here.
    __start = threading.Thread.start
    __join = threading.Thread.join

    def __init__(self, logger, level, pipe_out):
        """Initialize the LoggerPipe with the specified arguments."""

        threading.Thread.__init__(self)
        # Main thread should not call join() when exiting.
        self.daemon = True

        self.__logger = logger
        self.__level = level
        self.__pipe_out = pipe_out

        self.__lock = threading.Lock()
        self.__condition = threading.Condition(self.__lock)

        self.__started = False
        self.__finished = False

        LoggerPipe.__start(self)

    def start(self):
        """Start not implemented."""
        raise NotImplementedError("start should not be called directly")

    def run(self):
        """Read the output from 'pipe_out' and logs each line to 'logger'."""

        with self.__lock:
            self.__started = True
            self.__condition.notify_all()

        # Close the pipe when all of the output has been read.
        with self.__pipe_out:
            # Avoid buffering the output from the pipe.
            for line in iter(self.__pipe_out.readline, b""):
                # Convert the output of the process from a bytestring to a UTF-8 string, and replace
                # any characters that cannot be decoded with the official Unicode replacement
                # character, U+FFFD.
                line = line.decode("utf-8", "replace")
                self.__logger.log(self.__level, line.rstrip())

        with self.__lock:
            self.__finished = True
            self.__condition.notify_all()

    def join(self, timeout=None):
        """Join not implemented."""
        raise NotImplementedError("join should not be called directly")

    def wait_until_started(self):
        """Wait until started."""
        with self.__lock:
            while not self.__started:
                self.__condition.wait()

    def wait_until_finished(self):
        """Wait until finished."""
        with self.__lock:
            while not self.__finished:
                self.__condition.wait()

        # No need to pass a timeout to join() because the thread should already be done after
        # notifying us it has finished reading output from the pipe.
        LoggerPipe.__join(self)

def call(args, logger):
    """Call subprocess on args list."""
    logger.info(str(args))

    # Use a common pipe for stdout & stderr for logging.
    process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    logger_pipe = LoggerPipe(logger, logging.INFO, process.stdout)
    logger_pipe.wait_until_started()

    ret = process.wait()
    logger_pipe.wait_until_finished()

    if ret != 0:
        logger.error("Bad exit code %d", ret)
        raise Exception("Bad exit code %d from %s" % (ret, " ".join(args)))

def callo(args, logger):
    """Call subprocess on args string."""
    logger.info("%s", str(args))

    return subprocess.check_output(args)

def find_program(prog, paths):
    """Find the specified program in env PATH, or tries a set of paths."""
    loc = spawn.find_executable(prog)

    if loc is not None:
        return loc

    for loc in paths:
        full_prog = os.path.join(loc, prog)
        if os.path.exists(full_prog):
            return full_prog

    return None

def get_process_logger(debugger_output, pid, process_name):
    """Return the process logger from options specified."""
    process_logger = logging.Logger("process", level=logging.DEBUG)
    process_logger.mongo_process_filename = None

    if 'stdout' in debugger_output:
        s_handler = logging.StreamHandler(sys.stdout)
        s_handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        process_logger.addHandler(s_handler)

    if 'file' in debugger_output:
        filename = "debugger_%s_%d.log" % (os.path.splitext(process_name)[0], pid)
        process_logger.mongo_process_filename = filename
        f_handler = logging.FileHandler(filename=filename, mode="w")
        f_handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        process_logger.addHandler(f_handler)

    return process_logger

class WindowsDumper(object):
    """WindowsDumper class."""

    @staticmethod
    def __find_debugger(logger, debugger):
        """Find the installed debugger."""
        # We are looking for c:\Program Files (x86)\Windows Kits\8.1\Debuggers\x64.
        cdb = spawn.find_executable(debugger)
        if cdb is not None:
            return cdb
        from win32com.shell import shell, shellcon

        # Cygwin via sshd does not expose the normal environment variables.
        # Use the shell api to get the variable instead.
        root_dir = shell.SHGetFolderPath(0, shellcon.CSIDL_PROGRAM_FILESX86, None, 0)

        # Construct the debugger search paths in most-recent order.
        debugger_paths = [os.path.join(root_dir, "Windows Kits", "10", "Debuggers", "x64")]
        for idx in reversed(range(0, 2)):
            debugger_paths.append(
                os.path.join(root_dir, "Windows Kits", "8." + str(idx), "Debuggers", "x64"))

        for dbg_path in debugger_paths:
            logger.info("Checking for debugger in %s", dbg_path)
            if os.path.exists(dbg_path):
                return os.path.join(dbg_path, debugger)

        return None

    def dump_info(self, root_logger, logger, pid, process_name, take_dump):
        """Dump useful information to the console."""
        debugger = "cdb.exe"
        dbg = self.__find_debugger(root_logger, debugger)

        if dbg is None:
            root_logger.warning("Debugger %s not found, skipping dumping of %d", debugger, pid)
            return

        root_logger.info("Debugger %s, analyzing %s process with PID %d", dbg, process_name, pid)

        dump_command = ""
        if take_dump:
            # Dump to file, dump_<process name>.<pid>.mdmp.
            dump_file = "dump_%s.%d.%s" % (os.path.splitext(process_name)[0], pid,
                                           self.get_dump_ext())
            dump_command = ".dump /ma %s" % dump_file
            root_logger.info("Dumping core to %s", dump_file)

        cmds = [
            ".symfix",  # Fixup symbol path.
            "!sym noisy",  # Enable noisy symbol loading.
            ".symopt +0x10",  # Enable line loading (off by default in CDB, on by default in WinDBG).
            ".reload",  # Reload symbols.
            "!peb",  # Dump current exe & environment variables.
            "lm",  # Dump loaded modules.
            dump_command,
            "!uniqstack -pn",  # Dump all unique threads with function arguments.
            "!cs -l",  # Dump all locked critical sections.
            ".detach",  # Detach.
            "q"  # Quit.
        ]

        call([dbg, '-c', ";".join(cmds), '-p', str(pid)], logger)

        root_logger.info("Done analyzing %s process with PID %d", process_name, pid)

    @staticmethod
    def get_dump_ext():
        """Return the dump file extension."""
        return "mdmp"

class WindowsProcessList(object):
    """WindowsProcessList class."""

    @staticmethod
    def __find_ps():
        """Find tasklist."""
        return os.path.join(os.environ["WINDIR"], "system32", "tasklist.exe")

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]."""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s", ps)

        ret = callo([ps, "/FO", "CSV"], logger)

        buff = TextIOWrapper(BytesIO(ret))
        csv_reader = csv.reader(buff)

        return [[int(row[1]), row[0]] for row in csv_reader if row[1] != "PID"]

# LLDB dumper is for MacOS X.
class LLDBDumper(object):
    """LLDBDumper class."""

    @staticmethod
    def __find_debugger(debugger):
        """Find the installed debugger."""
        return find_program(debugger, ['/usr/bin'])

    def dump_info(self, root_logger, logger, pid, process_name, take_dump):
        """Dump info."""
        debugger = "lldb"
        dbg = self.__find_debugger(debugger)

        if dbg is None:
            root_logger.warning("Debugger %s not found, skipping dumping of %d", debugger, pid)
            return

        root_logger.info("Debugger %s, analyzing %s process with PID %d", dbg, process_name, pid)

        lldb_version = callo([dbg, "--version"], logger)

        logger.info(lldb_version)

        # Do we have the XCode or LLVM version of lldb?
        # Old versions of lldb do not work well when taking commands via a file.
        # XCode (7.2): lldb-340.4.119.
        # LLVM - lldb version 3.7.0 ( revision ).

        lldb_version = str(lldb_version)
        if 'version' not in lldb_version:
            # We have XCode's lldb.
            lldb_version = lldb_version[lldb_version.index("lldb-"):]
            lldb_version = lldb_version.replace('lldb-', '')
            lldb_major_version = int(lldb_version[:lldb_version.index('.')])
            if lldb_major_version < 340:
                logger.warning("Debugger lldb is too old, please upgrade to XCode 7.2")
                return

        dump_command = ""
        if take_dump:
            # Dump to file, dump_<process name>.<pid>.core.
            dump_file = "dump_%s.%d.%s" % (process_name, pid, self.get_dump_ext())
            dump_command = "process save-core %s" % dump_file
            root_logger.info("Dumping core to %s", dump_file)

        cmds = [
            "attach -p %d" % pid,
            "target modules list",
            "thread backtrace all",
            dump_command,
            "settings set interpreter.prompt-on-quit false",
            "quit",
        ]

        tf = tempfile.NamedTemporaryFile(mode='w', encoding='utf-8')

        for cmd in cmds:
            tf.write(cmd + "\n")

        tf.flush()

        # Works on in MacOS 10.9 & later.
        #call([dbg] +  list( itertools.chain.from_iterable([['-o', b] for b in cmds])), logger)
        call(['cat', tf.name], logger)
        call([dbg, '--source', tf.name], logger)

        root_logger.info("Done analyzing %s process with PID %d", process_name, pid)

    @staticmethod
    def get_dump_ext():
        """Return the dump file extension."""
        return "core"

class DarwinProcessList(object):
    """DarwinProcessList class."""

    @staticmethod
    def __find_ps():
        """Find ps."""
        return find_program('ps', ['/bin'])

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]."""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s", ps)

        ret = callo([ps, "-axco", "pid,comm"], logger)

        buff = TextIOWrapper(BytesIO(ret))
        csv_reader = csv.reader(buff, delimiter=' ', quoting=csv.QUOTE_NONE, skipinitialspace=True)

        return [[int(row[0]), row[1]] for row in csv_reader if row[0] != "PID"]

# GDB dumper is for Linux.
class GDBDumper(object):
    """GDBDumper class."""

    @staticmethod
    def __find_debugger(debugger):
        """Find the installed debugger."""
        return find_program(debugger, ['/opt/mongodbtoolchain/v3/bin/gdb', '/usr/bin'])

    def dump_info(self, root_logger, logger, pid, process_name, take_dump):
        """Dump info."""
        debugger = "gdb"
        dbg = self.__find_debugger(debugger)

        if dbg is None:
            logger.warning("Debugger %s not found, skipping dumping of %d", debugger, pid)
            return

        root_logger.info("Debugger %s, analyzing %s process with PID %d", dbg, process_name, pid)

        dump_command = ""
        if take_dump:
            # Dump to file, dump_<process name>.<pid>.core.
            dump_file = "dump_%s.%d.%s" % (process_name, pid, self.get_dump_ext())
            dump_command = "gcore %s" % dump_file
            root_logger.info("Dumping core to %s", dump_file)

        call([dbg, "--version"], logger)

        cmds = [
            "set interactive-mode off",
            "set print thread-events off",  # Suppress GDB messages of threads starting/finishing.
            "file %s" % process_name,
            "attach %d" % pid,
            "info sharedlibrary",
            "info threads",  # Dump a simple list of commands to get the thread name.
            "thread apply all bt",
            "set python print-stack full",
            # Lock the scheduler, before running commands, which execute code in the attached process.
            "set scheduler-locking on",
            dump_command,
            "set confirm off",
            "quit",
        ]

        call([dbg, "--quiet", "--nx"] +
             list(itertools.chain.from_iterable([['-ex', b] for b in cmds])), logger)

        root_logger.info("Done analyzing %s process with PID %d", process_name, pid)

    @staticmethod
    def get_dump_ext():
        """Return the dump file extension."""
        return "core"

    @staticmethod
    def _find_gcore():
        """Find the installed gcore."""
        dbg = "/usr/bin/gcore"
        if os.path.exists(dbg):
            return dbg

        return None

class LinuxProcessList(object):
    """LinuxProcessList class."""

    @staticmethod
    def __find_ps():
        """Find ps."""
        return find_program('ps', ['/bin', '/usr/bin'])

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]."""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s", ps)

        call([ps, "--version"], logger)

        ret = callo([ps, "-eo", "pid,args"], logger)

        buff = TextIOWrapper(BytesIO(ret))
        csv_reader = csv.reader(buff, delimiter=' ', quoting=csv.QUOTE_NONE, skipinitialspace=True)

        return [[int(row[0]), os.path.split(row[1])[1]] for row in csv_reader if row[0] != "PID"]

def get_hang_analyzers():
    """Return hang analyzers."""

    dbg = None
    ps = None

    # Skip taking the dump in Mac OS and result in an error.
    # FIXME : WT-6918 - Remove the skip block of code after fixing the issues.
    if sys.platform == "darwin":
        return [ps, dbg]

    if sys.platform.startswith("linux"):
        dbg = GDBDumper()
        ps = LinuxProcessList()
    elif _IS_WINDOWS or sys.platform == "cygwin":
        dbg = WindowsDumper()
        ps = WindowsProcessList()
    elif sys.platform == "darwin":
        dbg = LLDBDumper()
        ps = DarwinProcessList()

    return [ps, dbg]

def check_dump_quota(quota, ext):
    """Check if sum of the files with ext is within the specified quota in megabytes."""

    files = glob.glob("*." + ext)

    size_sum = 0
    for file_name in files:
        size_sum += os.path.getsize(file_name)

    return size_sum <= quota

def pname_match(exact_match, pname, processes):
    """Return True if the pname matches in processes."""
    pname = os.path.splitext(pname)[0]
    for ip in processes:
        if exact_match and pname == ip or not exact_match and ip in pname:
            return True
    return False

# Basic procedure
#
# 1. Get a list of interesting processes.
# 2. Dump useful information or take dumps.
def main():
    """Execute Main program."""
    root_logger = logging.Logger("hang_analyzer", level=logging.DEBUG)

    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(logging.Formatter(fmt="%(message)s"))
    root_logger.addHandler(handler)

    root_logger.info("Python Version: %s", sys.version)
    root_logger.info("OS: %s", platform.platform())

    try:
        if _IS_WINDOWS or sys.platform == "cygwin":
            distro = platform.win32_ver()
            root_logger.info("Windows Distribution: %s", distro)
        else:
            distro = platform.linux_distribution()
            root_logger.info("Linux Distribution: %s", distro)

    except AttributeError:
        root_logger.warning("Cannot determine Linux distro since Python is too old")

    try:
        uid = os.getuid()
        root_logger.info("Current User: %s", uid)
        current_login = os.getlogin()
        root_logger.info("Current Login: %s", current_login)
    except OSError:
        root_logger.warning("Cannot determine Unix Current Login")
    except AttributeError:
        root_logger.warning("Cannot determine Unix Current Login, not supported on Windows")

    contain_processes = ["ex_", "intpack-test", "python", "test_"]
    exact_processes = ["cursor_order", "packing-test", "t"]
    process_ids = []

    parser = OptionParser(description=__doc__)
    parser.add_option('-p', '--process-contains-names', dest='process_contains_names',
                      help='Comma separated list of process patterns to analyze')
    parser.add_option('-e', '--process-names', dest='process_exact_names',
                      help='Comma separated list of exact process names to analyze')
    parser.add_option('-d', '--process-ids', dest='process_ids', default=None,
                      help='Comma separated list of process ids (PID) to analyze, overrides -p & e')
    parser.add_option('-c', '--dump-core', dest='dump_core', action="store_true", default=False,
                      help='Dump core file for each analyzed process')
    parser.add_option('-s', '--max-core-dumps-size', dest='max_core_dumps_size', default=10000,
                      help='Maximum total size of core dumps to keep in megabytes')
    parser.add_option('-o', '--debugger-output', dest='debugger_output', action="append",
                      choices=['file', 'stdout'], default=None,
                      help="If 'stdout', then the debugger's output is written to the Python"
                      " process's stdout. If 'file', then the debugger's output is written"
                      " to a file named debugger_<process>_<pid>.log for each process it"
                      " attaches to. This option can be specified multiple times on the"
                      " command line to have the debugger's output written to multiple"
                      " locations. By default, the debugger's output is written only to the"
                      " Python process's stdout.")

    (options, _) = parser.parse_args()

    if options.debugger_output is None:
        options.debugger_output = ['stdout']

    if options.process_ids is not None:
        # process_ids is an int list of PIDs.
        process_ids = [int(pid) for pid in options.process_ids.split(',')]

    if options.process_exact_names is not None:
        exact_processes = options.process_exact_names.split(',')

    if options.process_contains_names is not None:
        contain_processes = options.process_contains_names.split(',')

    [ps, dbg] = get_hang_analyzers()

    if ps is None or dbg is None:
        root_logger.warning("hang_analyzer.py: Unsupported platform: %s", sys.platform)
        exit(1)

    all_processes = ps.dump_processes(root_logger)

    # Canonicalize the process names to lowercase to handle cases where the name of the Python
    # process is /System/Library/.../Python on OS X and -p python is specified.
    all_processes = [(pid, process_name.lower()) for (pid, process_name) in all_processes]

    # Find all running interesting processes:
    #   If a list of process_ids is supplied, match on that.
    #   Otherwise, do a substring match on interesting_processes.
    if process_ids:
        processes = [(pid, pname) for (pid, pname) in all_processes
                     if pid in process_ids and pid != os.getpid()]

        running_pids = set([pid for (pid, pname) in all_processes])
        missing_pids = set(process_ids) - running_pids
        if missing_pids:
            root_logger.warning("The following requested process ids are not running %s",
                                list(missing_pids))
    else:
        processes = [(pid, pname) for (pid, pname) in all_processes
                     if (pname_match(True, pname, exact_processes) or pname_match(False, pname, contain_processes)) and pid != os.getpid()]

    root_logger.info("Found %d interesting processes %s", len(processes), processes)

    max_dump_size_bytes = int(options.max_core_dumps_size) * 1024 * 1024

    trapped_exceptions = []

    # Dump all processes.
    for (pid, process_name) in processes:
        try:
            avoid_asan_dump(pid)
        except Exception as err:
            root_logger.warn("Error encountered when removing ASAN mappings from core dump", err)
            trapped_exceptions.append(traceback.format_exc())

        process_logger = get_process_logger(options.debugger_output, pid, process_name)
        try:
            dbg.dump_info(root_logger, process_logger, pid, process_name, options.dump_core
                          and check_dump_quota(max_dump_size_bytes, dbg.get_dump_ext()))
        except Exception as err:
            root_logger.info("Error encountered when invoking debugger %s", err)
            trapped_exceptions.append(traceback.format_exc())

    root_logger.info("Done analyzing all processes for hangs")

    for exception in trapped_exceptions:
        root_logger.info(exception)
    if trapped_exceptions:
        sys.exit(1)

# Remove the lowest bit from the core dump mask. The lowest bit is for
# anonymous private mappings (see: man core). These mappings are used by
# ASAN, MSAN et al to allocate shadow memory. Some of these mappings are
# correctly marked as "don't dump" via `memadvise`, but not all.
# Unfortunately these mappings are quite large (multiple terabytes), so
# don't write them to disk.
#
# In theory there could be WiredTiger use cases for these mappings, but as
# of writing there aren't any, and in any case a partial core dump is better
# than none because of an ENOSPC.
def avoid_asan_dump(pid):
    with open("/proc/%d/coredump_filter" % pid, "w+") as f:
        mask = int(f.read(), 16)
        mask &= 0xfffffffe
        f.write(str(hex(mask)))

if __name__ == "__main__":
    main()
