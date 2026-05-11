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

# Helper scripts for hazard pointers that can be called in gdb.
# Example: 
#     (in gdb) 
#     source hazard_pointers.py
#     dump_hazard_pointers

import gdb

# FIXME-WT-11929 - Move these type aliases and find_session helper function into their own files so other 
# scripts can use them.
SESSION_IMPL_PTR = gdb.lookup_type("WT_SESSION_IMPL").pointer()
CONN_IMPL_PTR = gdb.lookup_type("WT_CONNECTION_IMPL").pointer()

# Return a list of sessions and which thread they are in.
def find_sessions():
    original_frame = gdb.selected_frame()
    sessions = []

    current_prog = gdb.inferiors()[0]
    for thread in current_prog.threads():
        thread.switch()

        # Walk up the stack until we find a function that takes session as an argument.
        gdb.newest_frame()
        cur_frame = gdb.selected_frame()
        while cur_frame is not None:
            cur_frame.select()
            session_arg = gdb.execute("info arg session", to_string=True)
            if session_arg.startswith("session = 0x"):
                sessions.append((f"Thread {thread.global_num}", session_arg[10:].strip()))
                break
            cur_frame = cur_frame.older()

    # Return back to our original frame from before we called this function.
    original_frame.select()
    return sessions

# Helper script to dump all active hazard pointers and the sessions that hold them.
# Usage: `dump_hazard_pointers`
class dump_hazard_pointers(gdb.Command):
    def __init__(self):
        super(dump_hazard_pointers, self).__init__("dump_hazard_pointers", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        for (thread, session_ptr) in find_sessions():
            print(f"\n{thread}, session ({session_ptr}):")
            if(session_ptr == "0x0"):
                continue
            session = gdb.parse_and_eval(session_ptr).reinterpret_cast(SESSION_IMPL_PTR).dereference()
            if session['active']:
                for j in range(0, session['hazards']['inuse']):
                    hazard_ptr = session['hazards']['arr'][j]
                    if hazard_ptr['ref'] != 0:
                        print(f"    {hazard_ptr}")

# Helper script that lists all threads that hold a hazard pointer on the provided ref.
# Usage: `find_hazard_pointers_for 0xffff8c5792a0`
class find_hazard_pointer_for(gdb.Command):
    def __init__(self):
        super(find_hazard_pointer_for, self).__init__("find_hazard_pointers_for", gdb.COMMAND_DATA)

    def invoke(self, arg, from_tty):
        if(len(arg.split()) != 1 or not str(arg).startswith("0x")):
            print("Example usage: `find_hazard_pointers_for 0xffff8c5792a0``")
            return 1

        target_ref = str(arg)
        print(f"Threads, sessions with a hazard pointer on {target_ref}:")
        for (thread, session_ptr) in find_sessions():
            if(session_ptr == "0x0"):
                continue
            session = gdb.parse_and_eval(session_ptr).reinterpret_cast(SESSION_IMPL_PTR).dereference()
            if session['active']:
                for j in range(0, session['hazards']['inuse']):
                    hazard_ptr = session['hazards']['arr'][j]
                    if str(hazard_ptr['ref']) == str(target_ref):
                        print(f"    {thread}, Session {session_ptr}")
                        print(f"        {hazard_ptr}")

# Register scripts with gdb when `source` is called.
dump_hazard_pointers()
find_hazard_pointer_for()
