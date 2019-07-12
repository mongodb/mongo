"""GDB commands for MongoDB."""

import os
import re
import sys

import gdb

# pylint: disable=invalid-name,wildcard-import,broad-except
try:
    # Try to find and load the C++ pretty-printer library.
    import glob
    pp = glob.glob("/opt/mongodbtoolchain/v3/share/gcc-*/python/libstdcxx/v6/printers.py")
    printers = pp[0]
    path = os.path.dirname(os.path.dirname(os.path.dirname(printers)))
    sys.path.insert(0, path)
    from libstdcxx.v6 import register_libstdcxx_printers
    register_libstdcxx_printers(gdb.current_objfile())
    print("Loaded libstdc++ pretty printers from '%s'" % printers)
except Exception as e:
    print("Failed to load the libstdc++ pretty printers: " + str(e))
# pylint: enable=invalid-name,wildcard-import

if sys.version_info[0] < 3:
    raise gdb.GdbError(
        "MongoDB gdb extensions only support Python 3. Your GDB was compiled against Python 2")


def get_process_name():
    """Return the main binary we are attached to."""
    # The return from gdb.objfiles() could include the file extension of the debug symbols.
    main_binary_name = gdb.objfiles()[0].filename
    return os.path.splitext(os.path.basename(main_binary_name))[0]


def get_thread_id():
    """Return the thread_id of the current GDB thread."""
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


def get_current_thread_name():
    """Return the name of the current GDB thread."""
    fallback_name = '"%s"' % (gdb.selected_thread().name or '')
    try:
        # This goes through the pretty printer for StringData which adds "" around the name.
        name = str(gdb.parse_and_eval("mongo::for_debuggers::threadName"))
        if name == '""':
            return fallback_name
        return name
    except gdb.error:
        return fallback_name


def get_global_service_context():
    """Return the global ServiceContext object."""
    return gdb.parse_and_eval("'mongo::(anonymous namespace)::globalServiceContext'").dereference()


def get_session_catalog():
    """Return the global SessionCatalog object.

    Returns None if no SessionCatalog could be found.
    """
    # The SessionCatalog is a decoration on the ServiceContext.
    session_catalog_dec = get_decoration(get_global_service_context(), "mongo::SessionCatalog")
    if session_catalog_dec is None:
        return None
    return session_catalog_dec[1]


def get_decorations(obj):
    """Return an iterator to all decorations on a given object.

    Each object returned by the iterator is a tuple whose first element is the type name of the
    decoration and whose second element is the decoration object itself.

    TODO: De-duplicate the logic between here and DecorablePrinter. This code was copied from there.
    """
    type_name = str(obj.type).replace("class", "").replace(" ", "")
    decorable = obj.cast(gdb.lookup_type("mongo::Decorable<{}>".format(type_name)))
    decl_vector = decorable["_decorations"]["_registry"]["_decorationInfo"]
    start = decl_vector["_M_impl"]["_M_start"]
    finish = decl_vector["_M_impl"]["_M_finish"]

    decorable_t = decorable.type.template_argument(0)
    decinfo_t = gdb.lookup_type('mongo::DecorationRegistry<{}>::DecorationInfo'.format(
        str(decorable_t).replace("class", "").strip()))
    count = int((int(finish) - int(start)) / decinfo_t.sizeof)

    for i in range(count):
        descriptor = start[i]
        dindex = int(descriptor["descriptor"]["_index"])

        type_name = str(descriptor["constructor"])
        type_name = type_name[0:len(type_name) - 1]
        type_name = type_name[0:type_name.rindex(">")]
        type_name = type_name[type_name.index("constructAt<"):].replace("constructAt<", "")
        # get_unique_ptr should be loaded from 'mongo_printers.py'.
        decoration_data = get_unique_ptr(decorable["_decorations"]["_decorationData"])  # pylint: disable=undefined-variable

        if type_name.endswith('*'):
            type_name = type_name[0:len(type_name) - 1]
        type_name = type_name.rstrip()
        type_t = gdb.lookup_type(type_name)
        obj = decoration_data[dindex].cast(type_t)
        yield (type_name, obj)


def get_decoration(obj, type_name):
    """Find a decoration on 'obj' where the string 'type_name' is in the decoration's type name.

    Returns a tuple whose first element is the type name of the decoration and whose
    second is the decoration itself. If there are multiple such decorations, returns the first one
    that matches. Returns None if no matching decorations were found.
    """
    for dec_type_name, dec in get_decorations(obj):
        if type_name in dec_type_name:
            return (dec_type_name, dec)
    return None


def get_boost_optional(optional):
    """
    Retrieve the value stored in a boost::optional type, if it is non-empty.

    Returns None if the optional is empty.

    TODO: Import the boost pretty printers instead of using this custom function.
    """
    if not optional['m_initialized']:
        return None
    value_ref_type = optional.type.template_argument(0).pointer()
    storage = optional['m_storage']['dummy_']['data']
    return storage.cast(value_ref_type).dereference()


def get_field_names(value):
    """Return a list of all field names on a given GDB value."""
    return [typ.name for typ in value.type.fields()]


###################################################################################################
#
# Commands
#
###################################################################################################


class RegisterMongoCommand(object):
    """Class to register mongo commands with GDB."""

    _MONGO_COMMANDS = {}  # type: ignore

    @classmethod
    def register(cls, obj, name, command_class):
        """Register a command with no completer as a mongo command."""
        gdb.Command.__init__(obj, name, command_class)
        cls._MONGO_COMMANDS[name] = obj.__doc__

    @classmethod
    def print_commands(cls):
        """Print the registered mongo commands."""
        print("Command - Description")
        for key in cls._MONGO_COMMANDS:
            print("%s - %s" % (key, cls._MONGO_COMMANDS[key]))


class DumpGlobalServiceContext(gdb.Command):
    """Dump the Global Service Context."""

    def __init__(self):
        """Initialize DumpGlobalServiceContext."""
        RegisterMongoCommand.register(self, "mongodb-service-context", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):  # pylint: disable=no-self-use,unused-argument
        """Invoke GDB command to print the Global Service Context."""
        gdb.execute("print *('mongo::(anonymous namespace)::globalServiceContext')")


# Register command
DumpGlobalServiceContext()


class GetMongoDecoration(gdb.Command):
    """
    Search for a decoration on an object by typename and print it e.g.

    (gdb) mongo-decoration opCtx ReadConcernArgs

    would print out a decoration on opCtx whose type name contains the string "ReadConcernArgs".
    """

    def __init__(self):
        """Initialize GetMongoDecoration."""
        RegisterMongoCommand.register(self, "mongo-decoration", gdb.COMMAND_DATA)

    def invoke(self, args, _from_tty):  # pylint: disable=unused-argument,no-self-use
        """Invoke GetMongoDecoration."""
        argarr = args.split(" ")
        if len(argarr) < 2:
            raise ValueError("Must provide both an object and type_name argument.")

        # The object that is decorated.
        expr = argarr[0]
        # The substring of the decoration type that is to be printed.
        type_name_substr = argarr[1]
        dec = get_decoration(gdb.parse_and_eval(expr), type_name_substr)
        if dec:
            (type_name, obj) = dec
            print(type_name, obj)
        else:
            print("No decoration found whose type name contains '" + type_name_substr + "'.")


# Register command
GetMongoDecoration()


class DumpMongoDSessionCatalog(gdb.Command):
    """Print out the mongod SessionCatalog, which maintains a table of all Sessions.

    Prints out interesting information from TransactionParticipants too, which are decorations on
    the Session. If no arguments are provided, dumps out all sessions. Can optionally provide a
    session id argument. In that case, will only print the session for the specified id, if it is
    found. e.g.

    (gdb) dump-sessions "32cb9e84-98ad-4322-acf0-e055cad3ef73"

    """

    def __init__(self):
        """Initialize DumpMongoDSessionCatalog."""
        RegisterMongoCommand.register(self, "mongod-dump-sessions", gdb.COMMAND_DATA)

    def invoke(self, args, _from_tty):  # pylint: disable=unused-argument,no-self-use,too-many-locals,too-many-branches,too-many-statements
        """Invoke DumpMongoDSessionCatalog."""
        # See if a particular session id was specified.
        argarr = args.split(" ")
        lsid_to_find = None
        if argarr:
            lsid_to_find = argarr[0]

        # Get the SessionCatalog and the table of sessions.
        session_catalog = get_session_catalog()
        if session_catalog is None:
            print(
                "No SessionCatalog object was found on the ServiceContext. Not dumping any sessions."
            )
            return
        lsid_map = session_catalog["_sessions"]
        session_kv_pairs = list(absl_get_nodes(lsid_map))  # pylint: disable=undefined-variable
        print("Dumping %d Session objects from the SessionCatalog" % len(session_kv_pairs))

        # Optionally search for a specified session, based on its id.
        if lsid_to_find:
            print("Only printing information for session " + lsid_to_find + ", if found.")
            lsids_to_print = [lsid_to_find]
        else:
            lsids_to_print = [str(s['first']['_id']) for s in session_kv_pairs]

        for sess_kv in session_kv_pairs:
            # The Session is stored inside the SessionRuntimeInfo object.
            session_runtime_info = sess_kv['second']['_M_ptr'].dereference()
            session = session_runtime_info['session']
            # TODO: Add a custom pretty printer for LogicalSessionId.
            lsid_str = str(session['_sessionId']['_id'])

            # If we are only interested in a specific session, then we print out the entire Session
            # object, to aid more detailed debugging.
            if lsid_str == lsid_to_find:
                print("SessionId", "=", lsid_str)
                print(session)
                # Terminate if this is the only session we care about.
                break

            # Only print info for the necessary sessions.
            if lsid_str not in lsids_to_print:
                continue

            # If we are printing multiple sessions, we only print the most interesting fields from
            # the Session object for the sake of efficiency. We print the session id string first so
            # the session is easily identifiable.
            print("Session (" + str(session.address) + "):")
            print("SessionId", "=", lsid_str)
            session_fields_to_print = ['_checkoutOpCtx', '_killsRequested']
            for field in session_fields_to_print:
                # Skip fields that aren't found on the object.
                if field in get_field_names(session):
                    print(field, "=", session[field])
                else:
                    print("Could not find field '%s' on the Session object." % field)

            # Print the information from a TransactionParticipant if a session has one. Otherwise
            # we just print the session's id and nothing else.
            txn_part_dec = get_decoration(session, "TransactionParticipant")
            if txn_part_dec:
                # Only print the most interesting fields for debugging transactions issues. The
                # TransactionParticipant class encapsulates internal state in two distinct
                # structures: a 'PrivateState' type (stored in private field '_p') and an
                # 'ObservableState' type (stored in private field '_o'). The information we care
                # about here is all contained inside the 'ObservableState', so we extract fields
                # from that object. If, in the future, we want to print fields from the
                # 'PrivateState' object, we can inspect the TransactionParticipant's '_p' field.
                txn_part = txn_part_dec[1]
                txn_part_observable_state = txn_part['_o']
                fields_to_print = ['txnState', 'activeTxnNumber']
                print("TransactionParticipant (" + str(txn_part.address) + "):")
                for field in fields_to_print:
                    # Skip fields that aren't found on the object.
                    if field in get_field_names(txn_part_observable_state):
                        print(field, "=", txn_part_observable_state[field])
                    else:
                        print("Could not find field '%s' on the TransactionParticipant" % field)

                # The 'txnResourceStash' field is a boost::optional so we unpack it manually if it
                # is non-empty. We are only interested in its Locker object for now. TODO: Load the
                # boost pretty printers so the object will be printed clearly by default, without
                # the need for special unpacking.
                val = get_boost_optional(txn_part_observable_state['txnResourceStash'])
                if val:
                    locker_addr = get_unique_ptr(val["_locker"])  # pylint: disable=undefined-variable
                    locker_obj = locker_addr.dereference().cast(
                        gdb.lookup_type("mongo::LockerImpl"))
                    print('txnResourceStash._locker', "@", locker_addr)
                    print("txnResourceStash._locker._id", "=", locker_obj["_id"])
                else:
                    print('txnResourceStash', "=", None)
            # Separate sessions by a newline.
            print("")


# Register command
DumpMongoDSessionCatalog()


class MongoDBDumpLocks(gdb.Command):
    """Dump locks in mongod process."""

    def __init__(self):
        """Initialize MongoDBDumpLocks."""
        RegisterMongoCommand.register(self, "mongodb-dump-locks", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):  # pylint: disable=unused-argument
        """Invoke MongoDBDumpLocks."""
        print("Running Hang Analyzer Supplement - MongoDBDumpLocks")

        main_binary_name = get_process_name()
        if main_binary_name == 'mongod':
            self.dump_mongod_locks()
        else:
            print("Not invoking mongod lock dump for: %s" % (main_binary_name))

    @staticmethod
    def dump_mongod_locks():
        """GDB in-process python supplement."""

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
    """Print stack trace or a short message if the current thread is idle."""

    def __init__(self):
        """Initialize BtIfActive."""
        RegisterMongoCommand.register(self, "mongodb-bt-if-active", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):  # pylint: disable=no-self-use,unused-argument
        """Invoke GDB to print stack trace."""
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
    """Print unique stack traces of all threads in current process."""

    _HEADER_FORMAT = "Thread {gdb_thread_num}: {name} (Thread {pthread} (LWP {lwpid})):"

    def __init__(self):
        """Initialize MongoDBUniqueStack."""
        RegisterMongoCommand.register(self, "mongodb-uniqstack", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):
        """Invoke GDB to dump stacks."""
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

    @staticmethod
    def _process_thread_stack(arg, stacks, thread):
        """Process the thread stack."""
        thread_info = {}  # thread dict to hold per thread data
        thread_info['pthread'] = get_thread_id()
        thread_info['gdb_thread_num'] = thread.num
        thread_info['lwpid'] = thread.ptid[1]
        thread_info['name'] = get_current_thread_name()

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
        addrs_tuple = tuple(addrs)  # tuples are hashable, lists aren't.

        unique = stacks.setdefault(addrs_tuple, {'threads': []})
        unique['threads'].append(thread_info)
        if 'output' not in unique:
            try:
                unique['output'] = gdb.execute(arg, to_string=True).rstrip()
            except gdb.error as err:
                print("{} {}".format(thread_info['header'], err))

    @staticmethod
    def _dump_unique_stacks(stacks):
        """Dump the unique stacks."""

        def first_tid(stack):
            """Return the first tid."""
            return stack['threads'][0]['gdb_thread_num']

        for stack in sorted(list(stacks.values()), key=first_tid, reverse=True):
            for i, thread in enumerate(stack['threads']):
                prefix = '' if i == 0 else 'Duplicate '
                print(prefix + thread['header'])
            print(stack['output'])
            print()  # leave extra blank line after each thread stack


# Register command
MongoDBUniqueStack()


class MongoDBJavaScriptStack(gdb.Command):
    """Print the JavaScript stack from a MongoDB process."""

    def __init__(self):
        """Initialize MongoDBJavaScriptStack."""
        RegisterMongoCommand.register(self, "mongodb-javascript-stack", gdb.COMMAND_STATUS)

    def invoke(self, arg, _from_tty):  # pylint: disable=unused-argument
        """Invoke GDB to dump JS stacks."""
        print("Running Print JavaScript Stack Supplement")

        main_binary_name = get_process_name()
        if main_binary_name.endswith('mongod') or main_binary_name.endswith('mongo'):
            self.javascript_stack()
        else:
            print("No JavaScript stack print done for: %s" % (main_binary_name))

    @staticmethod
    def javascript_stack():
        """GDB in-process python supplement."""

        for thread in gdb.selected_inferior().threads():
            try:
                if not thread.is_valid():
                    print("Ignoring invalid thread %d in javascript_stack" % thread.num)
                    continue
                thread.switch()

                # Switch frames so gdb actually knows about the mongo::mozjs namespace. It doesn't
                # actually matter which frame so long as it isn't the top of the stack. This also
                # enables gdb to know about the mongo::mozjs::kCurrentScope thread-local variable
                # when using gdb.parse_and_eval().
                gdb.selected_frame().older().select()
            except gdb.error as err:
                print("Ignoring GDB error '%s' in javascript_stack" % str(err))
                continue

            try:
                if gdb.parse_and_eval(
                        'mongo::mozjs::kCurrentScope && mongo::mozjs::kCurrentScope->_inOp'):
                    gdb.execute('thread', from_tty=False, to_string=False)
                    gdb.execute(
                        'printf "%s\\n", ' +
                        'mongo::mozjs::kCurrentScope->buildStackString().c_str()', from_tty=False,
                        to_string=False)
            except gdb.error as err:
                print("Ignoring GDB error '%s' in javascript_stack" % str(err))
                continue


# Register command
MongoDBJavaScriptStack()


class MongoDBHelp(gdb.Command):
    """Dump list of mongodb commands."""

    def __init__(self):
        """Initialize MongoDBHelp."""
        gdb.Command.__init__(self, "mongodb-help", gdb.COMMAND_SUPPORT)

    def invoke(self, arg, _from_tty):  # pylint: disable=no-self-use,unused-argument
        """Register the mongo print commands."""
        RegisterMongoCommand.print_commands()


# Register command
MongoDBHelp()

print("MongoDB GDB commands loaded, run 'mongodb-help' for list of commands")
