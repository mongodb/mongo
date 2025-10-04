"""Script to be invoked by GDB for testing lock manager pretty printer."""

import traceback

import gdb

try:
    gdb.execute("break main")
    gdb.execute("run")
    gdb_type = lookup_type("mongo::LockManager")
    assert gdb_type is not None, "Failed to lookup type mongo::LockManager"
    gdb.write("TEST PASSED\n")
except Exception:
    gdb.write("TEST FAILED -- {!s}\n".format(traceback.format_exc()))
    gdb.execute("quit 1", to_string=True)
