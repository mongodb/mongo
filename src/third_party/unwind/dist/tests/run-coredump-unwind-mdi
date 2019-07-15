#!/bin/sh

# This test intends to test the unw_get_proc_name function on binaries without
# the symbol table but with so called MiniDebuginfo available. In particular,
# it is tested using the coredump accessors. For more info about MiniDebugInfo
# see e.g. http://fedoraproject.org/wiki/Features/MiniDebugInfo

${0%/*}/run-coredump-unwind -minidebuginfo
