#!/usr/bin/env python

"""Hang Analyzer

A prototype hang analyzer for Evergreen integration to help investigate test timeouts

1. Script supports taking dumps, and/or dumping a summary of useful information about a process
2. Script will iterate through a list of interesting processes,
    and run the tools from step 1. The list of processes can be provided as an option.
3. Java processes will be dumped using jstack, if available.

Supports Linux, MacOS X, Solaris, and Windows.
"""

import StringIO
import csv
import glob
import itertools
import logging
import os
import platform
import re
import signal
import subprocess
import sys
import tempfile
import traceback
import time
from distutils import spawn
from optparse import OptionParser
_is_windows = (sys.platform == "win32")

if _is_windows:
    import win32event
    import win32api


# Get relative imports to work when the package is not installed on the PYTHONPATH.
if __name__ == "__main__" and __package__ is None:
    sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from buildscripts.resmokelib import core


def call(a, logger):
    logger.info(str(a))

    # Use a common pipe for stdout & stderr for logging.
    process = subprocess.Popen(a, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    logger_pipe = core.pipe.LoggerPipe(logger, logging.INFO, process.stdout)
    logger_pipe.wait_until_started()

    ret = process.wait()
    logger_pipe.wait_until_finished()

    if ret != 0:
        logger.error("Bad exit code %d" % (ret))
        raise Exception("Bad exit code %d from %s" % (ret, " ".join(a)))


# Copied from python 2.7 version of subprocess.py
def check_output(*popenargs, **kwargs):
    r"""Run command with arguments and return its output as a byte string.

    If the exit code was non-zero it raises a CalledProcessError.  The
    CalledProcessError object will have the return code in the returncode
    attribute and output in the output attribute.

    The arguments are the same as for the Popen constructor.  Example:

    >>> check_output(["ls", "-l", "/dev/null"])
    'crw-rw-rw- 1 root root 1, 3 Oct 18  2007 /dev/null\n'

    The stdout argument is not allowed as it is used internally.
    To capture standard error in the result, use stderr=STDOUT.

    >>> check_output(["/bin/sh", "-c",
    ...               "ls -l non_existent_file ; exit 0"],
    ...              stderr=STDOUT)
    'ls: non_existent_file: No such file or directory\n'
    """
    if 'stdout' in kwargs:
        raise ValueError('stdout argument not allowed, it will be overridden.')
    process = subprocess.Popen(stdout=subprocess.PIPE, *popenargs, **kwargs)
    output, unused_err = process.communicate()
    retcode = process.poll()
    if retcode:
        cmd = kwargs.get("args")
        if cmd is None:
            cmd = popenargs[0]
        raise CalledProcessError(retcode, cmd, output=output)

    return output


def callo(a, logger):
    logger.info("%s" % str(a))

    return check_output(a)


def find_program(prog, paths):
    """Finds the specified program in env PATH, or tries a set of paths """
    loc = spawn.find_executable(prog)

    if loc is not None:
        return loc

    for loc in paths:
        p = os.path.join(loc, prog)
        if os.path.exists(p):
            return p

    return None


def get_process_logger(debugger_output, pid, process_name):
    """Returns the process logger from options specified."""
    process_logger = logging.Logger("process", level=logging.DEBUG)
    process_logger.mongo_process_filename = None

    if 'stdout' in debugger_output:
        handler = logging.StreamHandler(sys.stdout)
        handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        process_logger.addHandler(handler)

    if 'file' in debugger_output:
        filename = "debugger_%s_%d.log" % (os.path.splitext(process_name)[0], pid)
        process_logger.mongo_process_filename = filename
        handler = logging.FileHandler(filename=filename, mode="w")
        handler.setFormatter(logging.Formatter(fmt="%(message)s"))
        process_logger.addHandler(handler)

    return process_logger


class WindowsDumper(object):

    def __find_debugger(self, logger, debugger):
        """Finds the installed debugger"""
        # We are looking for c:\Program Files (x86)\Windows Kits\8.1\Debuggers\x64
        cdb = spawn.find_executable(debugger)
        if cdb is not None:
            return cdb
        from win32com.shell import shell, shellcon

        # Cygwin via sshd does not expose the normal environment variables
        # Use the shell api to get the variable instead
        rootDir = shell.SHGetFolderPath(0, shellcon.CSIDL_PROGRAM_FILESX86, None, 0)

        for i in range(0, 2):
            pathToTest = os.path.join(rootDir, "Windows Kits", "8." + str(i), "Debuggers", "x64")
            logger.info("Checking for debugger in %s" % pathToTest)
            if(os.path.exists(pathToTest)):
                return os.path.join(pathToTest, debugger)

        return None

    def dump_info(self, root_logger, logger, pid, process_name, take_dump):
        """Dump useful information to the console"""
        debugger = "cdb.exe"
        dbg = self.__find_debugger(root_logger, debugger)

        if dbg is None:
            root_logger.warning("Debugger %s not found, skipping dumping of %d" % (debugger, pid))
            return

        root_logger.info("Debugger %s, analyzing %s process with PID %d" % (dbg,
                                                                            process_name,
                                                                            pid))

        dump_command = ""
        if take_dump:
            # Dump to file, dump_<process name>.<pid>.mdmp
            dump_file = "dump_%s.%d.%s" % (os.path.splitext(process_name)[0],
                                           pid,
                                           self.get_dump_ext())
            dump_command = ".dump /ma %s" % dump_file
            root_logger.info("Dumping core to %s" % dump_file)

        cmds = [
            ".symfix",  # Fixup symbol path
            ".symopt +0x10",  # Enable line loading (off by default in CDB, on by default in WinDBG)
            ".reload",  # Reload symbols
            "!peb",     # Dump current exe, & environment variables
            "lm",       # Dump loaded modules
            dump_command,
            "!uniqstack -pn",  # Dump All unique Threads with function arguments
            "!cs -l",   # Dump all locked critical sections
            ".detach",  # Detach
            "q"         # Quit
            ]

        call([dbg, '-c', ";".join(cmds), '-p', str(pid)], logger)

        root_logger.info("Done analyzing %s process with PID %d" % (process_name, pid))

    def get_dump_ext(self):
        return "mdmp"


class WindowsProcessList(object):

    def __find_ps(self):
        """Finds tasklist """
        return os.path.join(os.environ["WINDIR"], "system32", "tasklist.exe")

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]"""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s" % ps)

        ret = callo([ps, "/FO", "CSV"], logger)

        b = StringIO.StringIO(ret)
        csvReader = csv.reader(b)

        p = [[int(row[1]), row[0]] for row in csvReader if row[1] != "PID"]

        return p


# LLDB dumper is for MacOS X
class LLDBDumper(object):

    def __find_debugger(self, debugger):
        """Finds the installed debugger"""
        return find_program(debugger, ['/usr/bin'])

    def dump_info(self, root_logger, logger, pid, process_name, take_dump):
        debugger = "lldb"
        dbg = self.__find_debugger(debugger)

        if dbg is None:
            root_logger.warning("Debugger %s not found, skipping dumping of %d" % (debugger, pid))
            return

        root_logger.info("Debugger %s, analyzing %s process with PID %d" % (dbg,
                                                                            process_name,
                                                                            pid))

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

        dump_command = ""
        if take_dump:
            # Dump to file, dump_<process name>.<pid>.core
            dump_file = "dump_%s.%d.%s" % (process_name, pid, self.get_dump_ext())
            dump_command = "process save-core %s" % dump_file
            root_logger.info("Dumping core to %s" % dump_file)

        cmds = [
            "attach -p %d" % pid,
            "target modules list",
            "thread backtrace all",
            dump_command,
            "settings set interpreter.prompt-on-quit false",
            "quit",
            ]

        tf = tempfile.NamedTemporaryFile()

        for c in cmds:
            tf.write(c + "\n")

        tf.flush()

        # Works on in MacOS 10.9 & later
        #call([dbg] +  list( itertools.chain.from_iterable([['-o', b] for b in cmds])), logger)
        call(['cat', tf.name], logger)
        call([dbg, '--source', tf.name], logger)

        root_logger.info("Done analyzing %s process with PID %d" % (process_name, pid))

    def get_dump_ext(self):
        return "core"


class DarwinProcessList(object):

    def __find_ps(self):
        """Finds ps"""
        return find_program('ps', ['/bin'])

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]"""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s" % ps)

        ret = callo([ps, "-axco", "pid,comm"], logger)

        b = StringIO.StringIO(ret)
        csvReader = csv.reader(b, delimiter=' ', quoting=csv.QUOTE_NONE, skipinitialspace=True)

        p = [[int(row[0]), row[1]] for row in csvReader if row[0] != "PID"]

        return p


# GDB dumper is for Linux & Solaris
class GDBDumper(object):

    def __find_debugger(self, debugger):
        """Finds the installed debugger"""
        return find_program(debugger, ['/opt/mongodbtoolchain/gdb/bin', '/usr/bin'])

    def dump_info(self, root_logger, logger, pid, process_name, take_dump):
        debugger = "gdb"
        dbg = self.__find_debugger(debugger)

        if dbg is None:
            logger.warning("Debugger %s not found, skipping dumping of %d" % (debugger, pid))
            return

        root_logger.info("Debugger %s, analyzing %s process with PID %d" % (dbg,
                                                                            process_name,
                                                                            pid))

        dump_command = ""
        if take_dump:
            # Dump to file, dump_<process name>.<pid>.core
            dump_file = "dump_%s.%d.%s" % (process_name, pid, self.get_dump_ext())
            dump_command = "gcore %s" % dump_file
            root_logger.info("Dumping core to %s" % dump_file)

        call([dbg, "--version"], logger)

        script_dir = os.path.dirname(os.path.abspath(__file__))
        root_logger.info("dir %s" % script_dir)
        gdb_dir = os.path.join(script_dir, "gdb")
        mongo_script = os.path.join(gdb_dir, "mongo.py")
        mongo_printers_script = os.path.join(gdb_dir, "mongo_printers.py")
        mongo_lock_script = os.path.join(gdb_dir, "mongo_lock.py")

        source_mongo = "source %s" % mongo_script
        source_mongo_printers = "source %s" % mongo_printers_script
        source_mongo_lock = "source %s" % mongo_lock_script
        mongodb_dump_locks = "mongodb-dump-locks"
        mongodb_show_locks = "mongodb-show-locks"
        mongodb_uniqstack = "mongodb-uniqstack mongodb-bt-if-active"
        mongodb_waitsfor_graph = "mongodb-waitsfor-graph debugger_waitsfor_%s_%d.gv" % \
            (process_name, pid)
        mongodb_javascript_stack = "mongodb-javascript-stack"

        # The following MongoDB python extensions do not run on Solaris.
        if sys.platform.startswith("sunos"):
            source_mongo_lock = ""
            # SERVER-28234 - GDB frame information not available on Solaris for a templatized
            # function
            mongodb_dump_locks = ""

            # SERVER-28373 - GDB thread-local variables not available on Solaris
            mongodb_show_locks = ""
            mongodb_waitsfor_graph = ""
            mongodb_javascript_stack = ""

        if not logger.mongo_process_filename:
            raw_stacks_commands = []
        else:
            base, ext = os.path.splitext(logger.mongo_process_filename)
            raw_stacks_filename = base + '_raw_stacks' + ext
            raw_stacks_commands = [
                'echo \\nWriting raw stacks to %s.\\n' % raw_stacks_filename,
                # This sends output to log file rather than stdout until we turn logging off.
                'set logging redirect on',
                'set logging file ' + raw_stacks_filename,
                'set logging on',
                'thread apply all bt',
                'set logging off',
                ]

        cmds = [
            "set interactive-mode off",
            "set print thread-events off",  # Python calls to gdb.parse_and_eval may cause threads
                                            # to start and finish. This suppresses those messages
                                            # from appearing in the return output.
            "file %s" % process_name,       # Solaris must load the process to read the symbols.
            "attach %d" % pid,
            "info sharedlibrary",
            "info threads",                 # Dump a simple list of commands to get the thread name
            "set python print-stack full",
            ] + raw_stacks_commands + [
            source_mongo,
            source_mongo_printers,
            source_mongo_lock,
            mongodb_uniqstack,
            "set scheduler-locking on",     # Lock the scheduler, before running any of the
                                            # following commands, which executes code in the
                                            # attached process.
            dump_command,
            mongodb_dump_locks,
            mongodb_show_locks,
            mongodb_waitsfor_graph,
            mongodb_javascript_stack,
            "set confirm off",
            "quit",
            ]

        call([dbg, "--quiet", "--nx"] +
             list(itertools.chain.from_iterable([['-ex', b] for b in cmds])),
             logger)

        root_logger.info("Done analyzing %s process with PID %d" % (process_name, pid))

    def get_dump_ext(self):
        return "core"

    def _find_gcore(self):
        """Finds the installed gcore"""
        dbg = "/usr/bin/gcore"
        if os.path.exists(dbg):
            return dbg

        return None


class LinuxProcessList(object):

    def __find_ps(self):
        """Finds ps"""
        return find_program('ps', ['/bin', '/usr/bin'])

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]"""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s" % ps)

        call([ps, "--version"], logger)

        ret = callo([ps, "-eo", "pid,args"], logger)

        b = StringIO.StringIO(ret)
        csvReader = csv.reader(b, delimiter=' ', quoting=csv.QUOTE_NONE, skipinitialspace=True)

        p = [[int(row[0]), os.path.split(row[1])[1]] for row in csvReader if row[0] != "PID"]

        return p


class SolarisProcessList(object):

    def __find_ps(self):
        """Finds ps"""
        return find_program('ps', ['/bin', '/usr/bin'])

    def dump_processes(self, logger):
        """Get list of [Pid, Process Name]"""
        ps = self.__find_ps()

        logger.info("Getting list of processes using %s" % ps)

        ret = callo([ps, "-eo", "pid,args"], logger)

        b = StringIO.StringIO(ret)
        csvReader = csv.reader(b, delimiter=' ', quoting=csv.QUOTE_NONE, skipinitialspace=True)

        p = [[int(row[0]), os.path.split(row[1])[1]] for row in csvReader if row[0] != "PID"]

        return p


# jstack is a JDK utility
class JstackDumper(object):

    def __find_debugger(self, debugger):
        """Finds the installed jstack debugger"""
        return find_program(debugger, ['/usr/bin'])

    def dump_info(self, root_logger, logger, pid, process_name):
        """Dump java thread stack traces to the console"""
        debugger = "jstack"
        jstack = self.__find_debugger(debugger)

        if jstack is None:
            logger.warning("Debugger %s not found, skipping dumping of %d" % (debugger, pid))
            return

        root_logger.info("Debugger %s, analyzing %s process with PID %d" % (jstack,
                                                                            process_name,
                                                                            pid))

        call([jstack, "-l", str(pid)], logger)

        root_logger.info("Done analyzing %s process with PID %d" % (process_name, pid))


# jstack is a JDK utility
class JstackWindowsDumper(object):

    def dump_info(self, root_logger, logger, pid, process_name):
        """Dump java thread stack traces to the logger"""

        root_logger.warning("Debugger jstack not supported, skipping dumping of %d" % (pid))


def get_hang_analyzers():
    dbg = None
    jstack = None
    ps = None
    if sys.platform.startswith("linux"):
        dbg = GDBDumper()
        jstack = JstackDumper()
        ps = LinuxProcessList()
    elif sys.platform.startswith("sunos"):
        dbg = GDBDumper()
        jstack = JstackDumper()
        ps = SolarisProcessList()
    elif _is_windows or sys.platform == "cygwin":
        dbg = WindowsDumper()
        jstack = JstackWindowsDumper()
        ps = WindowsProcessList()
    elif sys.platform == "darwin":
        dbg = LLDBDumper()
        jstack = JstackDumper()
        ps = DarwinProcessList()

    return [ps, dbg, jstack]


def check_dump_quota(quota, ext):
    """Check if sum of the files with ext is within the specified quota in megabytes"""

    files = glob.glob("*." + ext)

    size_sum = 0
    for file_name in files:
        size_sum += os.path.getsize(file_name)

    return (size_sum <= quota)


def signal_event_object(logger, pid):
    """Signal the Windows event object"""

    # Use unique event_name created.
    event_name = "Global\\Mongo_Python_" + str(pid)

    try:
        desired_access = win32event.EVENT_MODIFY_STATE
        inherit_handle = False
        task_timeout_handle = win32event.OpenEvent(desired_access,
                                                   inherit_handle,
                                                   event_name)
    except win32event.error as err:
        logger.info("Exception from win32event.OpenEvent with error: %s" % err)
        return

    try:
        win32event.SetEvent(task_timeout_handle)
    except win32event.error as err:
        logger.info("Exception from win32event.SetEvent with error: %s" % err)
    finally:
        win32api.CloseHandle(task_timeout_handle)

    logger.info("Waiting for process to report")
    time.sleep(5)


def signal_process(logger, pid, signalnum):
    """Signal process with signal, N/A on Windows"""
    try:
        os.kill(pid, signalnum)

        logger.info("Waiting for process to report")
        time.sleep(5)
    except OSError, e:
        logger.error("Hit OS error trying to signal process: %s" % str(e))

    except AttributeError:
        logger.error("Cannot send signal to a process on Windows")


def pname_match(match_type, pname, interesting_processes):
    pname = os.path.splitext(pname)[0]
    for ip in interesting_processes:
        if (match_type == 'exact' and pname == ip or
           match_type == 'contains' and ip in pname):
            return True
    return False


# Basic procedure
#
# 1. Get a list of interesting processes
# 2. Dump useful information or take dumps
def main():
    root_logger = logging.Logger("hang_analyzer", level=logging.DEBUG)

    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(logging.Formatter(fmt="%(message)s"))
    root_logger.addHandler(handler)

    root_logger.info("Python Version: %s" % sys.version)
    root_logger.info("OS: %s" % platform.platform())

    try:
        if _is_windows or sys.platform == "cygwin":
            distro = platform.win32_ver()
            root_logger.info("Windows Distribution: %s" % str(distro))
        else:
            distro = platform.linux_distribution()
            root_logger.info("Linux Distribution: %s" % str(distro))

    except AttributeError:
        root_logger.warning("Cannot determine Linux distro since Python is too old")

    try:
        uid = os.getuid()
        root_logger.info("Current User: %s" % str(uid))
        current_login = os.getlogin()
        root_logger.info("Current Login: %s" % current_login)
    except OSError:
        root_logger.warning("Cannot determine Unix Current Login")
    except AttributeError:
        root_logger.warning("Cannot determine Unix Current Login, not supported on Windows")

    interesting_processes = ["mongo", "mongod", "mongos", "_test", "dbtest", "python", "java"]
    go_processes = []
    process_ids = []

    parser = OptionParser(description=__doc__)
    parser.add_option('-m', '--process-match',
                      dest='process_match',
                      choices=['contains', 'exact'],
                      default='contains',
                      help="Type of match for process names (-p & -g), specify 'contains', or"
                           " 'exact'. Note that the process name match performs the following"
                           " conversions: change all process names to lowecase, strip off the file"
                           " extension, like '.exe' on Windows. Default is 'contains'.")
    parser.add_option('-p', '--process-names',
                      dest='process_names',
                      help='Comma separated list of process names to analyze')
    parser.add_option('-g', '--go-process-names',
                      dest='go_process_names',
                      help='Comma separated list of go process names to analyze')
    parser.add_option('-d', '--process-ids',
                      dest='process_ids',
                      default=None,
                      help='Comma separated list of process ids (PID) to analyze, overrides -p &'
                           ' -g')
    parser.add_option('-c', '--dump-core',
                      dest='dump_core',
                      action="store_true",
                      default=False,
                      help='Dump core file for each analyzed process')
    parser.add_option('-s', '--max-core-dumps-size',
                      dest='max_core_dumps_size',
                      default=10000,
                      help='Maximum total size of core dumps to keep in megabytes')
    parser.add_option('-o', '--debugger-output',
                      dest='debugger_output',
                      action="append",
                      choices=['file', 'stdout'],
                      default=None,
                      help="If 'stdout', then the debugger's output is written to the Python"
                           " process's stdout. If 'file', then the debugger's output is written"
                           " to a file named debugger_<process>_<pid>.log for each process it"
                           " attaches to. This option can be specified multiple times on the"
                           " command line to have the debugger's output written to multiple"
                           " locations. By default, the debugger's output is written only to the"
                           " Python process's stdout.")

    (options, args) = parser.parse_args()

    if options.debugger_output is None:
        options.debugger_output = ['stdout']

    if options.process_ids is not None:
        # process_ids is an int list of PIDs
        process_ids = [int(pid) for pid in options.process_ids.split(',')]

    if options.process_names is not None:
        interesting_processes = options.process_names.split(',')

    if options.go_process_names is not None:
        go_processes = options.go_process_names.split(',')
        interesting_processes += go_processes

    [ps, dbg, jstack] = get_hang_analyzers()

    if ps is None or (dbg is None and jstack is None):
        root_logger.warning("hang_analyzer.py: Unsupported platform: %s" % (sys.platform))
        exit(1)

    all_processes = ps.dump_processes(root_logger)

    # Canonicalize the process names to lowercase to handle cases where the name of the Python
    # process is /System/Library/.../Python on OS X and -p python is specified to hang_analyzer.py.
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
            root_logger.warning("The following requested process ids are not running %s" %
                                list(missing_pids))
    else:
        processes = [(pid, pname) for (pid, pname) in all_processes
                     if pname_match(options.process_match, pname, interesting_processes) and
                     pid != os.getpid()]

    root_logger.info("Found %d interesting processes %s" % (len(processes), processes))

    max_dump_size_bytes = int(options.max_core_dumps_size) * 1024 * 1024

    # Dump python processes by signalling them. The resmoke.py process will generate
    # the report.json, when signalled, so we do this before attaching to other processes.
    for (pid, process_name) in [(p, pn) for (p, pn) in processes if pn.startswith("python")]:
        # On Windows, we set up an event object to wait on a signal. For Cygwin, we register
        # a signal handler to wait for the signal since it supports POSIX signals.
        if _is_windows:
            root_logger.info("Calling SetEvent to signal python process %s with PID %d" %
                             (process_name, pid))
            signal_event_object(root_logger, pid)
        else:
            root_logger.info("Sending signal SIGUSR1 to python process %s with PID %d" %
                             (process_name, pid))
            signal_process(root_logger, pid, signal.SIGUSR1)

    trapped_exceptions = []

    # Dump all processes, except python & java.
    for (pid, process_name) in [(p, pn) for (p, pn) in processes
                                if not re.match("^(java|python)", pn)]:
        process_logger = get_process_logger(options.debugger_output, pid, process_name)
        try:
            dbg.dump_info(
                root_logger,
                process_logger,
                pid,
                process_name,
                options.dump_core and check_dump_quota(max_dump_size_bytes, dbg.get_dump_ext()))
        except Exception as err:
            root_logger.info("Error encountered when invoking debugger %s" % err)
            trapped_exceptions.append(traceback.format_exc())

    # Dump java processes using jstack.
    for (pid, process_name) in [(p, pn) for (p, pn) in processes if pn.startswith("java")]:
        process_logger = get_process_logger(options.debugger_output, pid, process_name)
        try:
            jstack.dump_info(root_logger, process_logger, pid, process_name)
        except Exception as err:
            root_logger.info("Error encountered when invoking debugger %s" % err)
            trapped_exceptions.append(traceback.format_exc())

    # Signal go processes to ensure they print out stack traces, and die on POSIX OSes.
    # On Windows, this will simply kill the process since python emulates SIGABRT as
    # TerminateProcess.
    # Note: The stacktrace output may be captured elsewhere (i.e. resmoke).
    for (pid, process_name) in [(p, pn) for (p, pn) in processes if pn in go_processes]:
        root_logger.info("Sending signal SIGABRT to go process %s with PID %d" %
                         (process_name, pid))
        signal_process(root_logger, pid, signal.SIGABRT)

    root_logger.info("Done analyzing all processes for hangs")

    for exception in trapped_exceptions:
        root_logger.info(exception)
    if trapped_exceptions:
        sys.exit(1)

if __name__ == "__main__":
    main()
