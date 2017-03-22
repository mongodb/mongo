"""GDB Pretty-printers and utility commands for MongoDB
"""
from __future__ import print_function

import gdb.printing
import os
import re
import sys

try:
    import bson
    import bson.json_util
    import collections
    from bson.codec_options import CodecOptions
except ImportError as e:
    print("Warning: Could not load bson library for Python '" + str(sys.version) + "'.")
    print("Check with the pip command if pymongo 3.x is installed.")
    bson = None


def get_unique_ptr(obj):
    """Read the value of a libstdc++ std::unique_ptr"""
    return obj["_M_t"]['_M_head_impl']


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
# Pretty-Printers
#
###################################################################################################


class StatusPrinter(object):
    """Pretty-printer for mongo::Status"""
    OK = 0  # ErrorCodes::OK

    def __init__(self, val):
        self.val = val

    def to_string(self):
        if not self.val['_error']:
            return 'Status::OK()'

        code = self.val['_error']['code']
        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        info = self.val['_error'].dereference()
        location = info['location']
        reason = info['reason']
        if location:
            return 'Status(%s, %s, %s)' % (code, reason, location)
        else:
            return 'Status(%s, %s)' % (code, reason)


class StatusWithPrinter:
    """Pretty-printer for mongo::StatusWith<>"""
    def __init__(self, val):
        self.val = val

    def to_string(self):
        if not self.val['_status']['_error']:
            return 'StatusWith(OK, %s)' % (self.val['_t'])

        code = self.val['_status']['_error']['code']

        # Remove the mongo::ErrorCodes:: prefix. Does nothing if not a real ErrorCode.
        code = str(code).split('::')[-1]

        info = self.val['_status']['_error'].dereference()
        location = info['location']
        reason = info['reason']
        if location:
            return 'StatusWith(%s, %s, %s)' % (code, reason, location)
        else:
            return 'StatusWith(%s, %s)' % (code, reason)


class StringDataPrinter:
    """Pretty-printer for mongo::StringData"""

    def __init__(self, val):
        self.val = val

    def display_hint(self):
        return 'string'

    def to_string(self):
        size = self.val["_size"]
        if size == -1:
            return self.val['_data'].lazy_string()
        else:
            return self.val['_data'].lazy_string(length=size)


class BSONObjPrinter:
    """Pretty-printer for mongo::BSONObj"""

    def __init__(self, val):
        self.val = val
        self.ptr = self.val['_objdata'].cast(gdb.lookup_type('void').pointer())
        self.size = self.ptr.cast(gdb.lookup_type('int').pointer()).dereference()

    def display_hint(self):
        return 'map'

    def children(self):
        if not bson:
            return

        inferior = gdb.selected_inferior()
        buf = bytes(inferior.read_memory(self.ptr, self.size))
        options = CodecOptions(document_class=collections.OrderedDict)
        bsondoc = bson.BSON.decode(buf, codec_options=options)

        for k, v in bsondoc.items():
            yield 'key', k
            yield 'value', bson.json_util.dumps(v)

    def to_string(self):
        ownership = "owned" if self.val['_ownedBuffer']['_buffer']['_holder']['px'] else "unowned"

        size = self.size
        if size < 5 or size > 17 * 1024 * 1024:
            size = hex(size)

        if size == 5:
            return "%s empty BSONObj @ %s" % (ownership, self.ptr)
        else:
            return "%s BSONObj %s bytes @ %s" % (ownership, size, self.ptr)


class UnorderedFastKeyTablePrinter:
    """Pretty-printer for mongo::UnorderedFastKeyTable<>"""

    def __init__(self, val):
        self.val = val

        # Get the value_type by doing a type lookup
        valueTypeName = val.type.strip_typedefs().name + "::value_type"
        valueType = gdb.lookup_type(valueTypeName).target()
        self.valueTypePtr = valueType.pointer()

    def display_hint(self):
        return 'map'

    def to_string(self):
        return "UnorderedFastKeyTablePrinter<%s> with %s elems " % (
            self.val.type.template_argument(0), self.val["_size"])

    def children(self):
        cap = self.val["_area"]["_hashMask"] + 1
        it = get_unique_ptr(self.val["_area"]["_entries"])
        end = it + cap

        if it == 0:
            return

        while it != end:
            elt = it.dereference()
            it += 1
            if not elt['_used']:
                continue

            value = elt['_data']["__data"].cast(self.valueTypePtr).dereference()

            yield ('key', value['first'])
            yield ('value', value['second'])


class DecorablePrinter:
    """Pretty-printer for mongo::Decorable<>"""

    def __init__(self, val):
        self.val = val

        decl_vector = val["_decorations"]["_registry"]["_decorationInfo"]
        # TODO: abstract out navigating a std::vector
        self.start = decl_vector["_M_impl"]["_M_start"]
        finish = decl_vector["_M_impl"]["_M_finish"]
        decinfo_t = gdb.lookup_type('mongo::DecorationRegistry::DecorationInfo')
        self.count = int((int(finish) - int(self.start)) / decinfo_t.sizeof)

    def display_hint(self):
        return 'map'

    def to_string(self):
        return "Decorable<%s> with %s elems " % (self.val.type.template_argument(0),
                                                 self.count)

    def children(self):
        decorationData = get_unique_ptr(self.val["_decorations"]["_decorationData"])

        for index in range(self.count):
            descriptor = self.start[index]
            dindex = int(descriptor["descriptor"]["_index"])

            # In order to get the type stored in the decorable, we examine the type of its
            # constructor, and do some string manipulations.
            # TODO: abstract out navigating a std::function
            type_name = str(descriptor["constructor"]["_M_functor"]["_M_unused"]["_M_object"])
            type_name = type_name[0:len(type_name) - 1]
            type_name = type_name[0: type_name.rindex(">")]
            type_name = type_name[type_name.index("constructAt<"):].replace("constructAt<", "")

            # If the type is a pointer type, strip the * at the end.
            if type_name.endswith('*'):
                type_name = type_name[0:len(type_name) - 1]
            type_name = type_name.rstrip()

            # Cast the raw char[] into the actual object that is stored there.
            type_t = gdb.lookup_type(type_name)
            obj = decorationData[dindex].cast(type_t)

            yield ('key', "%d:%s:%s" % (index, obj.address, type_name))
            yield ('value', obj)


def find_match_brackets(search, opening='<', closing='>'):
    """Returns the index of the closing bracket that matches the first opening bracket.
       Returns -1 if no last matching bracket is found, i.e. not a template.

       Example:
         'Foo<T>::iterator<U>''
         returns 5
    """
    index = search.find(opening)
    if index == -1:
        return -1

    start = index + 1
    count = 1
    str_len = len(search)
    for index in range(start, str_len):
        c = search[index]

        if c == opening:
            count += 1
        elif c == closing:
            count -= 1

        if count == 0:
            return index

    return -1


class MongoSubPrettyPrinter(gdb.printing.SubPrettyPrinter):
    """Sub pretty printer managed by the pretty-printer collection"""

    def __init__(self, name, prefix, is_template, printer):
        super(MongoSubPrettyPrinter, self).__init__(name)
        self.prefix = prefix
        self.printer = printer
        self.is_template = is_template


class MongoPrettyPrinterCollection(gdb.printing.PrettyPrinter):
    """MongoDB-specific printer printer collection that ignores subtypes.
    It will match 'HashTable<T> but not 'HashTable<T>::iterator' when asked for 'HashTable'.
    """

    def __init__(self):
        super(MongoPrettyPrinterCollection, self).__init__("mongo", [])

    def add(self, name, prefix, is_template, printer):
        self.subprinters.append(MongoSubPrettyPrinter(name, prefix, is_template, printer))

    def __call__(self, val):

        # Get the type name.
        lookup_tag = gdb.types.get_basic_type(val.type).tag
        if not lookup_tag:
            lookup_tag = val.type.name
        if not lookup_tag:
            return None

        index = find_match_brackets(lookup_tag)

        # Ignore subtypes of classes
        # We do not want HashTable<T>::iterator as an example, just HashTable<T>
        if index == -1 or index + 1 == len(lookup_tag):
            for printer in self.subprinters:
                if printer.enabled and (
                   (printer.is_template and lookup_tag.find(printer.prefix) == 0) or
                   (not printer.is_template and lookup_tag == printer.prefix)):
                    return printer.printer(val)

        return None


def build_pretty_printer():
    pp = MongoPrettyPrinterCollection()
    pp.add('BSONObj', 'mongo::BSONObj', False, BSONObjPrinter)
    pp.add('Decorable', 'mongo::Decorable', True, DecorablePrinter)
    pp.add('Status', 'mongo::Status', False, StatusPrinter)
    pp.add('StatusWith', 'mongo::StatusWith', True, StatusWithPrinter)
    pp.add('StringData', 'mongo::StringData', False, StringDataPrinter)
    pp.add('UnorderedFastKeyTable', 'mongo::UnorderedFastKeyTable', True, UnorderedFastKeyTablePrinter)
    return pp

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
            idle_location = None # If unsure, print a stack trace.

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

        addrs = [] # list of return addresses from frames
        frame = gdb.newest_frame()
        while frame:
            addrs.append(frame.pc())
            frame = frame.older()
        addrs = tuple(addrs) # tuples are hashable, lists aren't.

        unique = stacks.setdefault(addrs, {'threads': []})
        unique['threads'].append(thread_info)
        if 'output' not in unique:
            try:
                unique['output'] = gdb.execute(arg, to_string=True).rstrip()
            except gdb.error as err:
                raise gdb.GdbError("{} {}".format(thread_info['header'], err))

    def _dump_unique_stacks(self, stacks):
        def first_tid(stack):
            return stack['threads'][0]['gdb_thread_num']

        for stack in sorted(stacks.values(), key=first_tid, reverse=True):
            for i, thread in enumerate(stack['threads']):
                prefix = '' if i == 0 else 'Duplicate '
                print(prefix + thread['header'])
            print(stack['output'])
            print() # leave extra blank line after each thread stack

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

###################################################################################################
#
# Setup
#
###################################################################################################

# Register pretty-printers, replace existing mongo printers
gdb.printing.register_pretty_printer(
    gdb.current_objfile(),
    build_pretty_printer(),
    True)

print("MongoDB GDB pretty-printers and commands loaded, run 'mongodb-help' for list of commands")
