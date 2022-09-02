"""GDB commands for MongoDB."""

import datetime
import json
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
    from libstdcxx.v6 import printers as stdlib_printers
    register_libstdcxx_printers(gdb.current_objfile())
    print("Loaded libstdc++ pretty printers from '%s'" % printers)
except Exception as e:
    print("Failed to load the libstdc++ pretty printers: " + str(e))
# pylint: enable=invalid-name,wildcard-import

try:
    import bson
except ImportError as err:
    print("Warning: Could not load bson library for Python '" + str(sys.version) + "'.")
    print("Check with the pip command if pymongo 3.x is installed.")
    bson = None

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
        name = str(gdb.parse_and_eval("mongo::getThreadName()"))
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


def get_session_kv_pairs():
    """Return the SessionRuntimeInfoMap stored in the global SessionCatalog object.

    Returns a list of (LogicalSessionId, std::unique_ptr<SessionRuntimeInfo>) key-value pairs. For
    key-value pair 'session_kv', access the key with 'session_kv["first"]' and access the value with
    'session_kv["second"]'.
    """
    session_catalog = get_session_catalog()
    if session_catalog is None:
        return list()
    return list(absl_get_nodes(session_catalog["_sessions"]))  # pylint: disable=undefined-variable


def get_wt_session(recovery_unit, recovery_unit_impl_type):
    """Return the WT_SESSION pointer stored in the WiredTigerRecoveryUnit.

    Returns None if the recovery unit is not for the WiredTiger storage engine, or if no WT_SESSION
    has been opened.
    """

    if recovery_unit_impl_type != "mongo::WiredTigerRecoveryUnit":
        return None
    if not recovery_unit:
        return None
    wt_session_handle = get_unique_ptr(recovery_unit["_session"])  # pylint: disable=undefined-variable
    if not wt_session_handle.dereference().address:
        return None
    wt_session = wt_session_handle.dereference().cast(
        gdb.lookup_type("mongo::WiredTigerSession"))["_session"]  # pylint: disable=undefined-variable
    return wt_session


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
        try:
            type_t = gdb.lookup_type(type_name)
            obj = decoration_data[dindex].cast(type_t)
            yield (type_name, obj)
        except Exception as err:
            print("Failed to look up decoration type: " + type_name + ": " + str(err))


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

    def invoke(self, arg, _from_tty):  # pylint: disable=unused-argument
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

    def invoke(self, args, _from_tty):  # pylint: disable=unused-argument
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

    def invoke(self, args, _from_tty):  # pylint: disable=unused-argument
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
        session_kv_pairs = get_session_kv_pairs()
        print("Dumping %d Session objects from the SessionCatalog" % len(session_kv_pairs))

        # Optionally search for a specified session, based on its id.
        if lsid_to_find:
            print("Only printing information for session " + lsid_to_find + ", if found.")
            lsids_to_print = [lsid_to_find]
        else:
            lsids_to_print = [str(s['first']['_id']) for s in session_kv_pairs]

        for session_kv in session_kv_pairs:
            # The Session objects are stored inside the SessionRuntimeInfo object.
            session_runtime_info = get_unique_ptr(session_kv['second']).dereference()  # pylint: disable=undefined-variable
            parent_session = session_runtime_info['parentSession']
            child_sessions = absl_get_nodes(session_runtime_info['childSessions'])  # pylint: disable=undefined-variable
            lsid = str(parent_session['_sessionId']['_id'])

            # If we are only interested in a specific session, then we print out the entire Session
            # objects, to aid more detailed debugging.
            if lsid == lsid_to_find:
                self.dump_session_runtime_info(session_runtime_info)
                print(parent_session)
                for child_session_kv in child_sessions:
                    child_session = child_session_kv['second']
                    print(child_session)
                # Terminate if this is the only session we care about.
                break

            # Only print info for the necessary sessions.
            if lsid not in lsids_to_print:
                continue

            # If we are printing multiple sessions, we only print the most interesting fields for
            # each Session object for the sake of efficiency.
            self.dump_session_runtime_info(session_runtime_info)
            self.dump_session(parent_session)
            for child_session_kv in child_sessions:
                child_session = child_session_kv['second']
                self.dump_session(child_session)

    @staticmethod
    def dump_session_runtime_info(session_runtime_info):
        """Dump the session runtime info."""

        parent_session = session_runtime_info['parentSession']
        # TODO: Add a custom pretty printer for LogicalSessionId.
        lsid = str(parent_session['_sessionId']['_id'])[1:-1]
        print("SessionId =", lsid)
        fields_to_print = ['checkoutOpCtx', 'killsRequested']
        for field in fields_to_print:
            # Skip fields that aren't found on the object.
            if field in get_field_names(session_runtime_info):
                print(field, "=", session_runtime_info[field])
            else:
                print("Could not find field '%s' on the SessionRuntimeInfo object." % field)
        print("")

    @staticmethod
    def dump_session(session):
        """Dump the session."""

        print("Session (" + str(session.address) + "):")
        fields_to_print = ['_sessionId', '_numWaitingToCheckOut']
        for field in fields_to_print:
            # Skip fields that aren't found on the object.
            if field in get_field_names(session):
                print(field, "=", session[field])
            else:
                print("Could not find field '%s' on the SessionRuntimeInfo object." % field)

        # Print the information from a TransactionParticipant if a session has one.
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
            fields_to_print = ['txnState', 'activeTxnNumberAndRetryCounter']
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
                locker_obj = locker_addr.dereference().cast(gdb.lookup_type("mongo::LockerImpl"))
                print('txnResourceStash._locker', "@", locker_addr)
                print("txnResourceStash._locker._id", "=", locker_obj["_id"])
            else:
                print('txnResourceStash', "=", None)
        print("")


# Register command
DumpMongoDSessionCatalog()


class DumpMongoDBMutexes(gdb.Command):
    """Print out the state of mutexes in a mongodb (mongod or mongos) process."""

    def __init__(self):
        """Initialize DumpMongoDBMutexs."""
        RegisterMongoCommand.register(self, "mongodb-dump-mutexes", gdb.COMMAND_DATA)

    def invoke(self, args, _from_tty):  # pylint: disable=unused-argument
        """Invoke DumpMongoDBMutexes."""

        print("Dumping mutex info for all Clients")

        service_context = get_global_service_context()
        client_set = absl_get_nodes(service_context["_clients"])  # pylint: disable=undefined-variable
        for client_handle in client_set:
            client = client_handle.dereference().dereference()
            decoration_info = get_decoration(client, "DiagnosticInfoHandle")
            if not decoration_info:
                continue
            diagnostic_info_handle = decoration_info[1]
            diagnostic_info_list = diagnostic_info_handle["list"]

            # Use the STL pretty-printer to iterate over the list
            printer = stdlib_printers.StdForwardListPrinter(
                str(diagnostic_info_list.type), diagnostic_info_list)

            # Prepare structured output doc
            client_name = str(client["_desc"])
            # Chop off the "\"" from the beginning and end of the string
            client_name = client_name[1:-1]
            output_doc = {"client": client_name, "waiting": False}

            # This list will only ever have 0 or 1 element in it
            for _, diagnostic_info in printer.children():
                output_doc["waiting"] = True
                output_doc["mutex"] = str(diagnostic_info["_captureName"])[1:-1]

                millis = int(diagnostic_info["_timestamp"]["millis"])
                dt = datetime.datetime.fromtimestamp(millis / 1000, tz=datetime.timezone.utc)
                output_doc["since"] = dt.isoformat()
            print(json.dumps(output_doc))


# Register command
DumpMongoDBMutexes()


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
            gdb.execute("call mongo::getGlobalLockManager()->dump()", from_tty=False,
                        to_string=False)
        except gdb.error as gdberr:
            print("Ignoring error '%s' in dump_mongod_locks" % str(gdberr))


# Register command
MongoDBDumpLocks()


class MongoDBDumpRecoveryUnits(gdb.Command):
    """Dump recovery unit info for each client and session in a mongod process."""

    def __init__(self):
        """Initialize MongoDBDumpRecoveryUnits."""
        RegisterMongoCommand.register(self, "mongodb-dump-recovery-units", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):
        """Invoke MongoDBDumpRecoveryUnits."""
        print("Dumping recovery unit info for all clients and sessions")

        if not arg:
            arg = "mongo::WiredTigerRecoveryUnit"  # default to "mongo::WiredTigerRecoveryUnit"

        main_binary_name = get_process_name()
        if main_binary_name == "mongod":
            self.dump_recovery_units(arg)
        else:
            print("Not invoking mongod recovery unit dump for: %s" % (main_binary_name))

    @staticmethod
    def dump_recovery_units(recovery_unit_impl_type):
        """GDB in-process python supplement."""

        # Temporarily disable printing static members to make the output more readable
        out = gdb.execute("show print static-members", from_tty=False, to_string=True)
        enabled_at_start = False
        if out.startswith("Printing of C++ static members is on"):
            enabled_at_start = True
            gdb.execute("set print static-members off")

        # Dump active recovery unit info for each client in a mongod process
        service_context = get_global_service_context()
        client_set = absl_get_nodes(service_context["_clients"])  # pylint: disable=undefined-variable

        for client_handle in client_set:
            client = client_handle.dereference().dereference()

            # Prepare structured output doc
            client_name = str(client["_desc"])[1:-1]
            operation_context_handle = client["_opCtx"]
            output_doc = {"client": client_name, "opCtx": hex(operation_context_handle)}

            recovery_unit_handle = None
            recovery_unit = None
            if operation_context_handle:
                operation_context = operation_context_handle.dereference()
                recovery_unit_handle = get_unique_ptr(operation_context["_recoveryUnit"])  # pylint: disable=undefined-variable
                # By default, cast the recovery unit as "mongo::WiredTigerRecoveryUnit"
                recovery_unit = recovery_unit_handle.dereference().cast(
                    gdb.lookup_type(recovery_unit_impl_type))

            output_doc["recoveryUnit"] = hex(recovery_unit_handle) if recovery_unit else "0x0"
            wt_session = get_wt_session(recovery_unit, recovery_unit_impl_type)
            if wt_session:
                output_doc["WT_SESSION"] = hex(wt_session)
            print(json.dumps(output_doc))
            if recovery_unit:
                print(recovery_unit)

        # Dump stashed recovery unit info for each session in a mongod process
        for session_kv in get_session_kv_pairs():
            # The Session objects are stored inside the SessionRuntimeInfo object.
            session_runtime_info = get_unique_ptr(session_kv['second']).dereference()  # pylint: disable=undefined-variable
            parent_session = session_runtime_info['parentSession']
            child_sessions = absl_get_nodes(session_runtime_info['childSessions'])  # pylint: disable=undefined-variable

            MongoDBDumpRecoveryUnits.dump_session(parent_session, recovery_unit_impl_type)
            for child_session_kv in child_sessions:
                child_session = child_session_kv['second']
                MongoDBDumpRecoveryUnits.dump_session(child_session, recovery_unit_impl_type)

        if enabled_at_start:
            gdb.execute("set print static-members on")

    @staticmethod
    def dump_session(session, recovery_unit_impl_type):
        """Dump the session."""

        # Prepare structured output doc
        lsid = session["_sessionId"]
        output_doc = {"session": str(lsid["_id"])[1:-1], "txnResourceStash": "0x0"}
        txn_participant_dec = get_decoration(session, "TransactionParticipant")
        recovery_unit_handle = None
        recovery_unit = None

        if txn_participant_dec:
            txn_participant_observable_state = txn_participant_dec[1]["_o"]
            txn_resource_stash = get_boost_optional(
                txn_participant_observable_state["txnResourceStash"])
            if txn_resource_stash:
                output_doc["txnResourceStash"] = str(txn_resource_stash.address)
                recovery_unit_handle = get_unique_ptr(txn_resource_stash["_recoveryUnit"])  # pylint: disable=undefined-variable
                # By default, cast the recovery unit as "mongo::WiredTigerRecoveryUnit"
                recovery_unit = recovery_unit_handle.dereference().cast(
                    gdb.lookup_type(recovery_unit_impl_type))

        output_doc["recoveryUnit"] = hex(recovery_unit_handle) if recovery_unit else "0x0"
        wt_session = get_wt_session(recovery_unit, recovery_unit_impl_type)
        if wt_session:
            output_doc["WT_SESSION"] = hex(wt_session)

        print(json.dumps(output_doc))
        print(lsid)
        if recovery_unit:
            print(recovery_unit)


# Register command
MongoDBDumpRecoveryUnits()


class MongoDBDumpStorageEngineInfo(gdb.Command):
    """Dump storage engine info in mongod process."""

    def __init__(self):
        """Initialize MongoDBDumpStorageEngineInfo."""
        RegisterMongoCommand.register(self, "mongodb-dump-storage-engine-info", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):  # pylint: disable=unused-argument
        """Invoke MongoDBDumpStorageEngineInfo."""
        print("Running Hang Analyzer Supplement - MongoDBDumpStorageEngineInfo")

        main_binary_name = get_process_name()
        if main_binary_name == 'mongod':
            self.dump_mongod_storage_engine_info()
        else:
            print("Not invoking mongod storage engine info dump for: %s" % (main_binary_name))

    @staticmethod
    def dump_mongod_storage_engine_info():
        """GDB in-process python supplement."""

        try:
            # Call into mongod, and dump the state of storage engine
            # Note that output will go to mongod's standard output, not the debugger output window
            gdb.execute(
                "call mongo::getGlobalServiceContext()->_storageEngine._ptr._value._M_b._M_p->dump()",
                from_tty=False, to_string=False)
        except gdb.error as gdberr:
            print("Ignoring error '%s' in dump_mongod_storage_engine_info" % str(gdberr))


# Register command
MongoDBDumpStorageEngineInfo()


class BtIfActive(gdb.Command):
    """Print stack trace or a short message if the current thread is idle."""

    def __init__(self):
        """Initialize BtIfActive."""
        RegisterMongoCommand.register(self, "mongodb-bt-if-active", gdb.COMMAND_DATA)

    def invoke(self, arg, _from_tty):  # pylint: disable=unused-argument
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

    # Looking to test your changes to this? Really easy!
    # 1. install-jstestshell to build the mongo shell binary (mongo)
    # 2. launch it: ./path/to/bin/mongo --nodb
    # 3. in the shell, run: sleep(99999999999). (do not use --eval)
    # 4. ps ax | grep nodb to find the PID
    # 5. gdb -p <PID>.
    # 6. Run this command, mongodb-javascript-stack

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
    def atomic_get_ptr(atomic_scope: gdb.Value):
        """Fetch the underlying pointer from std::atomic."""

        # Awkwardly, the gdb.Value type does not support a check like
        # `'_M_b' in atomic_scope`, so exceptions for flow control it is. :|
        try:
            # reach into std::atomic and grab the pointer. This is for libc++
            return atomic_scope['_M_b']['_M_p']
        except gdb.error:
            # Worst case scenario: try and use .load(), but it's probably
            # inlined. parse_and_eval required because you can't call methods
            # in gdb on the Python API
            return gdb.parse_and_eval(
                f"((std::atomic<mongo::mozjs::MozJSImplScope*> *)({atomic_scope.address}))->load()")

        return None

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
                # The following block is roughly equivalent to this:
                # namespace mongo::mozjs {
                #   std::atomic<MozJSImplScope*> kCurrentScope = ...;
                # }
                # if (!scope || scope->_inOp == 0) { continue; }
                # print(scope->buildStackString()->c_str());
                atomic_scope = gdb.parse_and_eval("mongo::mozjs::kCurrentScope")
                ptr = MongoDBJavaScriptStack.atomic_get_ptr(atomic_scope)
                if not ptr:
                    continue

                scope = ptr.dereference()
                if scope['_inOp'] == 0:
                    continue

                gdb.execute('thread', from_tty=False, to_string=False)
                # gdb continues to not support calling methods through Python,
                # so work around it by casting the raw ptr back to its type,
                # and calling the method through execute darkness
                gdb.execute(
                    f'printf "%s\\n", ((mongo::mozjs::MozJSImplScope*)({ptr}))->buildStackString().c_str()',
                    from_tty=False, to_string=False)

            except gdb.error as err:
                print("Ignoring GDB error '%s' in javascript_stack" % str(err))
                continue


# Register command
MongoDBJavaScriptStack()


class MongoDBPPrintBsonAtPointer(gdb.Command):
    """Interprets a pointer into raw memory as the start of a bson object. Pretty print the results."""

    def __init__(self):
        """Init."""
        RegisterMongoCommand.register(self, "mongodb-pprint-bson", gdb.COMMAND_STATUS)

    def invoke(self, args, _from_tty):
        """Invoke."""
        args = args.split(' ')
        if len(args) == 0 or (len(args) == 1 and len(args[0]) == 0):
            print("Usage: mongodb-pprint-bson <ptr> <optional length>")
            return

        ptr = eval(args[0])  # pylint: disable=eval-used
        size = 20 * 1024
        if len(args) >= 2:
            size = int(args[1])
        print("Pretty printing bson object at %s (%d bytes)" % (ptr, size))

        memory = gdb.selected_inferior().read_memory(ptr, size).tobytes()
        bsonobj = next(bson.decode_iter(memory))

        from pprint import pprint
        pprint(bsonobj)


MongoDBPPrintBsonAtPointer()


class MongoDBHelp(gdb.Command):
    """Dump list of mongodb commands."""

    def __init__(self):
        """Initialize MongoDBHelp."""
        gdb.Command.__init__(self, "mongodb-help", gdb.COMMAND_SUPPORT)

    def invoke(self, arg, _from_tty):  # pylint: disable=unused-argument
        """Register the mongo print commands."""
        RegisterMongoCommand.print_commands()


# Register command
MongoDBHelp()

print("MongoDB GDB commands loaded, run 'mongodb-help' for list of commands")
