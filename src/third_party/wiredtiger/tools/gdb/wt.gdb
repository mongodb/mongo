# Usage: source <path_to>/wt.gdb
# This is a thin wrapper around the other files that hold actual helper functions.

# Assume GDB is run from a build directory one level deep in the tree.
# TODO: Find a better solution
directory ../tools/gdb_commands

# Source the known gdb helpers
source dump_row_int.gdb
source wt_debug_script_update.py

