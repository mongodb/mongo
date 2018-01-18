"""GDB commands for MongoDB
"""
from __future__ import print_function

import gdb
import os
import re
import sys


def get_process_name():
    """Return the main binary we are attached to."""
    # The return from gdb.objfiles() could include the file extension of the debug symbols.
    main_binary_name = gdb.objfiles()[0].filename
    return os.path.splitext(os.path.basename(main_binary_name))[0]


def get_thread_id():
    """Returns the thread_id of the current GDB thread"""
    # GDB thread example:
    #  RHEL
    #   [Current thread is 1 (Thread 0x7f072426cca0 (LWP 12867))]
    thread_info = gdb.execute("thread", from_tty=False, to_string=True)

    if sys.platform.startswith("linux"):
        match = re.search(r"Thread (?P<pthread_id>0x[0-9a-f]+)", thread_info)
        if match:
            return int(match.group("pthread_id"), 16)
    elif sys.platform.startswith("sunos"):
        match = re.search(r"Thread (?P<pthread_id>[0-9]+)", thread_info)
        if match:
            return int(match.group("pthread_id"), 10)
        lwpid = gdb.selected_thread().ptid[1]
        if lwpid != 0:
            return lwpid
    raise ValueError("Failed to find thread id in {}".format(thread_info))


###################################################################################################
#
# Commands
#
###################################################################################################
# Dictionary of commands so we can write a help function that describes the MongoDB commands.
mongo_commands = {}


def register_mongo_command(obj, name, command_class):
    """Register a command with no completer as a mongo command"""
    global mongo_commands
    gdb.Command.__init__(obj, name, command_class)

    mongo_commands[name] = obj.__doc__


class DumpGlobalServiceContext(gdb.Command):
    """Dump the Global Service Context"""

    def __init__(self):
        register_mongo_command(self, "mongodb-service-context", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):
        gdb.execute("print *('mongo::(anonymous namespace)::globalServiceContext')")

# Register command
DumpGlobalServiceContext()


class MongoDBDumpLocks(gdb.Command):
    """Dump locks in mongod process"""

    def __init__(self):
        register_mongo_command(self, "mongodb-dump-locks", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):
        print("Running Hang Analyzer Supplement - MongoDBDumpLocks")

        main_binary_name = get_process_name()
        if main_binary_name == 'mongod':
            self.dump_mongod_locks()
        else:
            print("Not invoking mongod lock dump for: %s" % (main_binary_name))

    def dump_mongod_locks(self):
        """GDB in-process python supplement"""

        try:
            # Call into mongod, and dump the state of lock manager
            # Note that output will go to mongod's standard output, not the debugger output window
            gdb.execute("call ('mongo::(anonymous namespace)::globalLockManager').dump()",
                        from_tty=False, to_string=False)
        except gdb.error as gdberr:
            print("Ignoring error '%s' in dump_mongod_locks" % str(gdberr))

# Register command
MongoDBDumpLocks()


class BtIfActive(gdb.Command):
    """Print stack trace or a short message if the current thread is idle"""

    def __init__(self):
        register_mongo_command(self, "mongodb-bt-if-active", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):
        try:
            idle_location = gdb.parse_and_eval("mongo::for_debuggers::idleThreadLocation")
        except gdb.error:
            idle_location = None  # If unsure, print a stack trace.

        if idle_location:
            print("Thread is idle at " + idle_location.string())
        else:
            gdb.execute("bt")

# Register command
BtIfActive()


class MongoDBUniqueStack(gdb.Command):
    """Print unique stack traces of all threads in current process"""

    _HEADER_FORMAT = "Thread {gdb_thread_num}: {name} (Thread {pthread} (LWP {lwpid})):"

    def __init__(self):
        register_mongo_command(self, "mongodb-uniqstack", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):
        stacks = {}
        if not arg:
            arg = 'bt'  # default to 'bt'

        current_thread = gdb.selected_thread()
        try:
            for thread in gdb.selected_inferior().threads():
                if not thread.is_valid():
                    continue
                thread.switch()
                self._process_thread_stack(arg, stacks, thread)
            self._dump_unique_stacks(stacks)
        finally:
            if current_thread and current_thread.is_valid():
                current_thread.switch()

    def _get_current_thread_name(self):
        fallback_name = '"%s"' % (gdb.selected_thread().name or '')
        try:
            # This goes through the pretty printer for StringData which adds "" around the name.
            name = str(gdb.parse_and_eval("mongo::for_debuggers::threadName"))
            if name == '""':
                return fallback_name
            return name
        except gdb.error:
            return fallback_name

    def _process_thread_stack(self, arg, stacks, thread):
        thread_info = {}  # thread dict to hold per thread data
        thread_info['pthread'] = get_thread_id()
        thread_info['gdb_thread_num'] = thread.num
        thread_info['lwpid'] = thread.ptid[1]
        thread_info['name'] = self._get_current_thread_name()

        if sys.platform.startswith("linux"):
            header_format = "Thread {gdb_thread_num}: {name} (Thread 0x{pthread:x} (LWP {lwpid}))"
        elif sys.platform.startswith("sunos"):
            (_, _, thread_tid) = thread.ptid
            if thread_tid != 0 and thread_info['lwpid'] != 0:
                header_format = "Thread {gdb_thread_num}: {name} (Thread {pthread} (LWP {lwpid}))"
            elif thread_info['lwpid'] != 0:
                header_format = "Thread {gdb_thread_num}: {name} (LWP {lwpid})"
            else:
                header_format = "Thread {gdb_thread_num}: {name} (Thread {pthread})"
        else:
            raise ValueError("Unsupported platform: {}".format(sys.platform))
        thread_info['header'] = header_format.format(**thread_info)

        addrs = []  # list of return addresses from frames
        frame = gdb.newest_frame()
        while frame:
            addrs.append(frame.pc())
            try:
                frame = frame.older()
            except gdb.error as err:
                print("{} {}".format(thread_info['header'], err))
                break
        addrs = tuple(addrs)  # tuples are hashable, lists aren't.

        unique = stacks.setdefault(addrs, {'threads': []})
        unique['threads'].append(thread_info)
        if 'output' not in unique:
            try:
                unique['output'] = gdb.execute(arg, to_string=True).rstrip()
            except gdb.error as err:
                print("{} {}".format(thread_info['header'], err))

    def _dump_unique_stacks(self, stacks):
        def first_tid(stack):
            return stack['threads'][0]['gdb_thread_num']

        for stack in sorted(stacks.values(), key=first_tid, reverse=True):
            for i, thread in enumerate(stack['threads']):
                prefix = '' if i == 0 else 'Duplicate '
                print(prefix + thread['header'])
            print(stack['output'])
            print()  # leave extra blank line after each thread stack

# Register command
MongoDBUniqueStack()


class MongoDBJavaScriptStack(gdb.Command):
    """Print the JavaScript stack from a MongoDB process"""

    def __init__(self):
        register_mongo_command(self, "mongodb-javascript-stack", gdb.COMMAND_STATUS)

    def invoke(self, arg, _from_tty):
        print("Running Print JavaScript Stack Supplement")

        main_binary_name = get_process_name()
        if main_binary_name.endswith('mongod') or main_binary_name.endswith('mongo'):
            self.javascript_stack()
        else:
            print("No JavaScript stack print done for: %s" % (main_binary_name))

    def javascript_stack(self):
        """GDB in-process python supplement"""

        for thread in gdb.selected_inferior().threads():
            try:
                if not thread.is_valid():
                    print("Ignoring invalid thread %d in javascript_stack" % thread.num)
                    continue
                thread.switch()
            except gdb.error as err:
                print("Ignoring GDB error '%s' in javascript_stack" % str(err))
                continue

            try:
                if gdb.parse_and_eval(
                        'mongo::mozjs::kCurrentScope && mongo::mozjs::kCurrentScope->_inOp'):
                    gdb.execute('thread', from_tty=False, to_string=False)
                    gdb.execute('printf "%s\n", ' +
                                'mongo::mozjs::kCurrentScope->buildStackString().c_str()',
                                from_tty=False, to_string=False)
            except gdb.error as err:
                print("Ignoring GDB error '%s' in javascript_stack" % str(err))
                continue


# Register command
MongoDBJavaScriptStack()


class MongoDBHelp(gdb.Command):
    """Dump list of mongodb commands"""

    def __init__(self):
        gdb.Command.__init__(self, "mongodb-help", gdb.COMMAND_SUPPORT)

    def invoke(self, arg, _from_tty):
        print("Command - Description")
        for key in mongo_commands:
            print("%s - %s" % (key, mongo_commands[key]))

# Register command
MongoDBHelp()

print("MongoDB GDB commands loaded, run 'mongodb-help' for list of commands")
