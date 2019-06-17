import lldb


def __lldb_init_module(debugger, dict):
    ############################
    # register custom commands #
    ############################
    debugger.HandleCommand(
        "command script add -f lldb_commands.PrintGlobalServiceContext mongodb-service-context")
    debugger.HandleCommand(
        "command script add -f lldb_commands.PrintGlobalServiceContext mongodb-dump-locks")
    debugger.HandleCommand("command alias mongodb-help help")


#######################
# Command Definitions #
#######################


def PrintGlobalServiceContext(debugger, *args):
    """
    Provides the same convenience command available in GDB
    integrations to print the globalServiceContext.
    """
    debugger.HandleCommand("print *globalServiceContext")


def MongoDBDumpLocks(debugger, *args):
    """Dump locks in the mongod process."""
    debugger.HandleCommand("call globalLockManager.dump()")
