"""Add user-defined commands to MongoDB."""

import argparse
import shlex


def __lldb_init_module(debugger, *_args):
    """Register custom commands."""
    debugger.HandleCommand(
        "command script add -o -f lldb_commands.PrintGlobalServiceContext mongodb-service-context"
    )
    debugger.HandleCommand(
        "command script add -o -f lldb_commands.PrintGlobalServiceContext mongodb-dump-locks"
    )
    debugger.HandleCommand(
        "command script add -o -f lldb_commands.BreakpointOnAssert mongodb-breakpoint-assert"
    )
    debugger.HandleCommand(
        "command script add -o -f lldb_commands.MongoDBFindBreakpoint mongodb-find-breakpoint"
    )
    debugger.HandleCommand("command script add -o -f lldb_commands.DumpGSC mongodb-gsc")
    debugger.HandleCommand("command alias mongodb-help help")


#######################
# Command Definitions #
#######################


def PrintGlobalServiceContext(debugger, *_args):
    """Provide the mongodb-service-context command.

    Emulates the same convenience command available in GDB
    integrations to print the globalServiceContext.
    """
    debugger.HandleCommand("print *globalServiceContext")


def MongoDBDumpLocks(debugger, *_args):
    """Dump locks in the mongod process."""
    debugger.HandleCommand("call mongo::dumpLockManager()")


def BreakpointOnAssert(debugger, command, _exec_ctx, _result, _internal_dict):
    """Set a breakpoint on MongoDB uassert that throws the specified error code."""

    arg_strs = shlex.split(command)

    parser = argparse.ArgumentParser(description="Set a breakpoint on a usassert code.")
    parser.add_argument("code", metavar="N", type=int, help="uassert code")
    args = parser.parse_args(arg_strs)

    debugger.HandleCommand(
        'breakpoint set -n mongo::uassertedWithLocation -c "(int)status._error.px->code == %s"'
        % args.code
    )


def MongoDBFindBreakpoint(debugger, _command, exec_ctx, _result, _internal_dict):
    """Find the thread that triggered a breakpoint from 'debugger.cpp'."""

    process = exec_ctx.process

    print("Threads: %d" % (len(process.threads)))
    thread_num = 0
    for thread_index, thread in enumerate(process.threads):
        frame_count = min(thread.num_frames, 10)
        for frame_index in range(frame_count):
            frame_str = thread.frames[frame_index].__str__()

            # Find the frame that has a call to `execCallback` the function `src/mongo/util/debugger.cpp` uses
            if "execCallback" in frame_str:
                thread_num = thread_index + 1
                break

        if thread_num:
            break

    print("Switching thread to thread that hit breakpoint: %s" % (thread_num))
    debugger.HandleCommand("thread select %d" % (thread_num))


def DumpGSC(_debugger, _command, exec_ctx, _result, _internal_dict):
    """Dump the global service context as a hash table."""

    gsc_list = exec_ctx.target.FindGlobalVariables("globalServiceContext", 1)
    print(gsc_list)
    gsc = gsc_list[0]
    decorations = gsc.GetChildMemberWithName("_decorations")
    registry = decorations.GetChildMemberWithName("_registry")
    decoration_info = registry.GetChildMemberWithName("_decorationInfo")
    decoration_data = decorations.GetChildMemberWithName("_decorationData").child[0]

    print(decoration_info.num_children)
    for child in range(decoration_info.num_children):
        di = decoration_info.children[child]
        constructor = di.GetChildMemberWithName("constructor").__str__()
        index = (
            di.GetChildMemberWithName("descriptor")
            .GetChildMemberWithName("_index")
            .GetValueAsUnsigned()
        )

        type_name = constructor
        type_name = type_name[0 : len(type_name) - 1]
        type_name = type_name[0 : type_name.rindex(">")]
        type_name = type_name[type_name.index("constructAt<") :].replace("constructAt<", "")

        # If the type is a pointer type, strip the * at the end.
        if type_name.endswith("*"):
            type_name = type_name[0 : len(type_name) - 1]
        type_name = type_name.rstrip()

        type_t = exec_ctx.target.FindTypes(type_name).GetTypeAtIndex(0)
        offset_ptr = decoration_data.GetChildAtIndex(index, False, True)

        value = offset_ptr.Cast(type_t)
        print(value)
