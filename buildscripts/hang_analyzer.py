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
import itertools
import os
import platform
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
from distutils import spawn
from optparse import OptionParser

if sys.platform == "win32":
    import win32process

def call(a = []):
    sys.stdout.write(str(a) + "\n")
    sys.stdout.flush()
    ret = subprocess.call(a)
    if( ret != 0):
        sys.stderr.write("Bad exit code %d\n" % (ret))
        raise Exception()

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

def callo(a = []):
    sys.stdout.write(str(a) + "\n")
    sys.stdout.flush()
    return check_output(a)

def find_program(prog, paths):
    """Finds the specified program in env PATH, or tries a set of paths """
    loc = spawn.find_executable(prog)

    if(loc != None):
        return loc

    for loc in paths:
        p = os.path.join(loc, prog)
        if os.path.exists(p):
            return p

    return None


class WindowsDumper(object):

    def __find_debugger(self):
        """Finds the installed debugger"""
        # We are looking for c:\Program Files (x86)\Windows Kits\8.1\Debuggers\x64
        cdb = spawn.find_executable('cdb.exe')
        if(cdb != None):
            return cdb
        from win32com.shell import shell, shellcon

        # Cygwin via sshd does not expose the normal environment variables
        # Use the shell api to get the variable instead
        rootDir = shell.SHGetFolderPath(0, shellcon.CSIDL_PROGRAM_FILESX86, None, 0)

        for i in range(0,2):
            pathToTest = os.path.join(rootDir, "Windows Kits", "8." + str(i), "Debuggers", "x64" )
            sys.stdout.write("Checking for debugger in %s\n" % pathToTest)
            if(os.path.exists(pathToTest)):
                return os.path.join(pathToTest, "cdb.exe")
        return None

    def dump_info(self, pid, process_name, stream):
        """Dump useful information to the console"""
        dbg = self.__find_debugger()

        if dbg is None:
            stream.write("WARNING: Debugger cdb.exe not found, skipping dumping of %d\n" % (pid))
            return

        stream.write("INFO: Debugger %s, analyzing %d\n" % (dbg, pid))

        cmds = [
            ".symfix",  # Fixup symbol path
            ".reload",  # Reload symbols
            "!peb",     # Dump current exe, & environment variables
            "lm",       # Dump loaded modules
            "~* k 100", # Dump All Threads
            ".dump /ma /u dump_" + process_name + ".mdmp",
                        # Dump to file, dump_<process name>_<time stamp>_<pid in hex>.mdmp
            ".detach",  # Detach
            "q"         # Quit
            ]

        call([dbg, '-c', ";".join(cmds), '-p', str(pid)])

        stream.write("INFO: Done analyzing process\n")

    def dump_core(self, pid, output_file):
        """Take a dump of pid to specified file"""
        dbg = self.__find_debugger()

        if dbg is None:
            sys.stdout.write("WARNING: Debugger cdb.exe not found, skipping dumping of %d to %s\n" % (pid, output_file))
            return

        sys.stdout.write("INFO: Debugger %s, analyzing %d to %s\n" % (dbg, pid, output_file))

        call([dbg, '-c', ".dump /ma %s;.detach;q" % output_file, '-p', str(pid)] )

        sys.stdout.write("INFO: Done analyzing process\n")

class WindowsProcessList(object):

    def __find_ps(self):
        """Finds tasklist """
        return os.path.join(os.environ["WINDIR"], "system32", "tasklist.exe")

    def dump_processes(self):
        """Get list of [Pid, Process Name]"""
        ps = self.__find_ps()

        sys.stdout.write("INFO: Getting list of processes using %s\n" % ps)

        ret = callo([ps, "/FO", "CSV"])

        b = StringIO.StringIO(ret)
        csvReader = csv.reader(b)

        p = [[int(row[1]), row[0]] for row in csvReader if row[1] != "PID"]

        sys.stdout.write("INFO: Done analyzing process\n")

        return p

# LLDB dumper is for MacOS X
class LLDBDumper(object):

    def __find_debugger(self):
        """Finds the installed debugger"""
        return find_program('lldb', ['/usr/bin'])

    def dump_info(self, pid, process_name, stream):
        dbg = self.__find_debugger()

        if dbg is None:
            stream.write("WARNING: Debugger lldb not found, skipping dumping of %d\n" % (pid))
            return

        stream.write("INFO: Debugger %s, analyzing %d\n" % (dbg, pid))

        lldb_version = callo([dbg, "--version"])

        stream.write(lldb_version)

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
                stream.write("WARNING: Debugger lldb is too old, please upgrade to XCode 7.2\n")
                return

        cmds = [
            "attach -p %d" % pid,
            "target modules list",
            "thread backtrace all",
            "settings set interpreter.prompt-on-quit false",
            "quit",
            ]

        tf = tempfile.NamedTemporaryFile()

        for c in cmds:
            tf.write(c + "\n")

        tf.flush()

        # Works on in MacOS 10.9 & later
        #call([dbg] +  list( itertools.chain.from_iterable([['-o', b] for b in cmds])))
        call(['cat', tf.name])
        call([dbg, '--source', tf.name])

        stream.write("INFO: Done analyzing process\n")

    def dump_core(self, pid, output_file):
        """Take a dump of pid to specified file"""
        sys.stderr.write("ERROR: lldb does not support dumps, stupid debugger\n")

class DarwinProcessList(object):

    def __find_ps(self):
        """Finds ps"""
        return find_program('ps', ['/bin'])

    def dump_processes(self):
        """Get list of [Pid, Process Name]"""
        ps = self.__find_ps()

        sys.stdout.write("INFO: Getting list of processes using %s\n" % ps)

        ret = callo([ps, "-axco", "pid,comm"])

        b = StringIO.StringIO(ret)
        csvReader = csv.reader(b, delimiter=' ', quoting=csv.QUOTE_NONE, skipinitialspace=True)

        p = [[int(row[0]), row[1]] for row in csvReader if row[0] != "PID"]

        sys.stdout.write("INFO: Done analyzing process\n")

        return p

# GDB dumper is for Linux & Solaris
class GDBDumper(object):

    def __find_debugger(self):
        """Finds the installed debugger"""
        return find_program('gdb', ['/opt/mongodbtoolchain/bin', '/usr/bin'])

    def dump_info(self, pid, process_name, stream):
        dbg = self.__find_debugger()

        if dbg is None:
            stream.write("WARNING: Debugger gdb not found, skipping dumping of %d\n" % (pid))
            return

        stream.write("INFO: Debugger %s, analyzing %d\n" % (dbg, pid))

        call([dbg, "--version"])

        cmds = [
            "attach %d" % pid,
            "thread apply all bt",
            "set confirm off",
            "quit",
            ]

        call([dbg, "--quiet"] + list( itertools.chain.from_iterable([['-ex', b] for b in cmds])))

        stream.write("INFO: Done analyzing process\n")

    def _find_gcore(self):
        """Finds the installed gcore"""
        dbg = "/usr/bin/gcore"
        if os.path.exists(dbg):
            return dbg
        return None

    def dump_core(self, pid, output_file):
        """Take a dump of pid to specified file"""

        dbg = self._find_gcore()

        if dbg is None:
            sys.stdout.write("WARNING: Debugger gcore not found, skipping dumping of %d to %s\n" % (pid, output_file))
            return

        sys.stdout.write("INFO: Debugger %s, analyzing %d to %s\n" % (dbg, pid, output_file))

        call([dbg, "-o", output_file, str(pid)])

        sys.stdout.write("INFO: Done analyzing process\n")

        # GCore appends the pid to the output file name
        return output_file + "." + str(pid)

class LinuxProcessList(object):
    def __find_ps(self):
        """Finds ps"""
        return find_program('ps', ['/bin', '/usr/bin'])

    def dump_processes(self):
        """Get list of [Pid, Process Name]"""
        ps = self.__find_ps()

        sys.stdout.write("INFO: Getting list of processes using %s\n" % ps)

        call([ps, "--version"])

        ret = callo([ps, "-eo", "pid,args"])

        b = StringIO.StringIO(ret)
        csvReader = csv.reader(b, delimiter=' ', quoting=csv.QUOTE_NONE, skipinitialspace=True)

        p = [[int(row[0]), os.path.split(row[1])[1]] for row in csvReader if row[0] != "PID"]

        sys.stdout.write("INFO: Done analyzing process\n")

        return p

class SolarisProcessList(object):
    def __find_ps(self):
        """Finds ps"""
        return find_program('ps', ['/bin', '/usr/bin'])

    def dump_processes(self):
        """Get list of [Pid, Process Name]"""
        ps = self.__find_ps()

        sys.stdout.write("INFO: Getting list of processes using %s\n" % ps)

        ret = callo([ps, "-eo", "pid,args"])

        b = StringIO.StringIO(ret)
        csvReader = csv.reader(b, delimiter=' ', quoting=csv.QUOTE_NONE, skipinitialspace=True)

        p = [[int(row[0]), os.path.split(row[1])[1]] for row in csvReader if row[0] != "PID"]

        sys.stdout.write("INFO: Done analyzing process\n")

        return p

# jstack is a JDK utility
class JstackDumper(object):

    def __find_debugger(self):
        """Finds the installed jstack debugger"""
        return find_program('jstack', ['/usr/bin'])

    def dump_info(self, pid, process_name, stream):
        """Dump java thread stack traces to the console"""
        jstack = self.__find_debugger()

        if jstack is None:
            stream.write("WARNING: Debugger jstack not found, skipping dumping of %d\n" % (pid))
            return

        stream.write("INFO: Debugger %s, analyzing %d\n" % (jstack, pid))

        call([jstack, "-l", str(pid)])

        stream.write("INFO: Done analyzing process\n")

    def dump_core(self, pid, output_file):
        """Take a dump of pid to specified file"""
        sys.stderr.write("ERROR: jstack does not support dumps\n")


# jstack is a JDK utility
class JstackWindowsDumper(object):

    def dump_info(self, pid, process_name, stream):
        """Dump java thread stack traces to the console"""

        stream.write("WARNING: Debugger jstack not supported, skipping dumping of %d\n" % (pid))

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
    elif os.name == 'nt' or (os.name == "posix" and sys.platform == "cygwin"):
        dbg = WindowsDumper()
        jstack = JstackWindowsDumper()
        ps = WindowsProcessList()
    elif sys.platform == "darwin":
        dbg = LLDBDumper()
        jstack = JstackDumper()
        ps = DarwinProcessList()
    return [ps, dbg, jstack]


def signal_process(pid):
    """Signal python process with SIGUSR1, N/A on Windows"""
    try:
        os.kill(pid, signal.SIGUSR1)

        print "Waiting for python process to report"
        time.sleep(5)
    except OSError,e:
        print "Hit OS error trying to signal python process: " + str(e)

    except AttributeError:
        print "Cannot send signal to python on Windows"


def timeout_protector():
    print "Script timeout has been hit, terminating"
    if sys.platform == "win32":
        # Have the process exit with code 9 when it terminates itself to closely match the exit code
        # of the process when it sends itself a SIGKILL.
        handle = win32process.GetCurrentProcess()
        win32process.TerminateProcess(handle, 9)
    else:
        os.kill(os.getpid(), signal.SIGKILL)


# Basic procedure
#
# 1. Get a list of interesting processes
# 2. Dump useful information or take dumps
def main():
    print "Python Version: " + sys.version
    print "OS: " + platform.platform()

    try:
        distro = platform.linux_distribution()
        print "Linux Distribution: " + str(distro)
    except AttributeError:
        print "Cannot determine Linux distro since Python is too old"

    try:
        uid = os.getuid()
        print "Current User: " + str(uid)
        current_login = os.getlogin()
        print "Current Login: " + current_login
    except OSError:
        print "Cannot determine Unix Current Login"
    except AttributeError:
        print "Cannot determine Unix Current Login, not supported on Windows"

    interesting_processes = ["mongo", "mongod", "mongos", "_test", "dbtest", "python", "java"]
    parser = OptionParser(description=__doc__)
    parser.add_option('-p', '--process_names', dest='process_names', help='List of process names to analyze')
    (options, args) = parser.parse_args()
    if options.process_names is not None:
        interesting_processes = options.process_names.split(',')

    [ps, dbg, jstack] = get_hang_analyzers()

    if( ps == None or (dbg == None and jstack == None)):
        sys.stderr.write("hang_analyzer.py: Unsupported platform: %s\n" % (sys.platform))
        exit(1)

    # Make sure the script does not hang
    timer = threading.Timer(120, timeout_protector)
    timer.start()

    processes_orig = ps.dump_processes()

    # Find all running interesting processes.
    processes = [a for a in processes_orig
                    if any([a[1].find(ip) >= 0 for ip in interesting_processes]) and a[0] != os.getpid()]

    sys.stdout.write("Found %d interesting processes\n" % len(processes))

    if( len(processes) == 0):
        for process in processes_orig:
            sys.stdout.write("Ignoring process %d of %s\n" % (process[0], process[1]))
    else:
        # Dump all other processes, except python & java.
        for process in [a for a in processes if not re.match("^(java|python)", a[1])]:
            sys.stdout.write("Dumping process %d of %s\n" % (process[0], process[1]))
            dbg.dump_info(process[0], process[1], sys.stdout)

        # Dump java processes using jstack.
        for process in [a for a in processes if a[1].startswith("java")]:
            sys.stdout.write("Dumping process %d of %s\n" % (process[0], process[1]))
            jstack.dump_info(process[0], process[1], sys.stdout)

        # Dump python processes after signalling them.
        for process in [a for a in processes if a[1].startswith("python")]:
            signal_process(process[0])

            dbg.dump_info(process[0], process[1], sys.stdout)

    # Suspend the timer so we can exit cleanly
    timer.cancel()

    sys.stdout.write("Done analyzing processes for hangs\n")

if __name__ == "__main__":
    main()
