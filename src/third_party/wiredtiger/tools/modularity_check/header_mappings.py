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

# Header files in include/ are often associated with module up the src/ folder.
# This dict provided a mapping from file names in src/include to modules in the src/ directory.
header_mappings = {
    "api.h": "conn",
    "bitstring_inline.h": "support",
    "bitstring.h": "support",
    "btmem.h": "btree",
    "btree_cmp_inline.h": "btree",
    "buf_inline.h": "include",
    "cache_inline.h": "evict",
    "cache.h": "evict",
    "capacity.h": "include",
    "cell_inline.h": "reconcile",
    "cell.h": "reconcile",
    "checkpoint.h": "btree",
    "column_inline.h": "btree",
    "compact.h": "btree",
    "conf_keys.h": "conf",
    "connection.h": "conn",
    "ctype_inline.h": "include",
    "dhandle.h": "btree",
    "dlh.h": "include",
    "error.h": "include",
    "futex.h": "os_layer",
    "gcc.h": "include",
    "generation.h": "support",
    "hardware.h": "include",
    "hazard.h": "support",
    "intpack_inline.h": "packing",
    "json.h": "support",
    "misc_inline.h": "include",
    "misc.h": "include",
    "modify_inline.h": "btree",
    "msvc.h": "include",
    "mutex_inline.h": "support",
    "mutex.h": "support",
    "os_fhandle_inline.h": "os_layer",
    "os_fs_inline.h": "os_layer",
    "os_fstream_inline.h": "os_layer",
    "os_windows.h": "os_layer",
    "os.h": "os_layer",
    "pagestat_inline.h": "include",
    "posix.h": "os_layer",
    "queue.h": "include",
    "ref_inline.h": "btree",
    "serial_inline.h": "btree",
    "stat.h": "support",
    "str_inline.h": "support",
    "swap.h": "include",
    "thread_group.h": "support",
    "time_inline.h": "support",
    "timestamp_inline.h": "support",
    "timestamp.h": "support",
    "truncate.h": "txn",
    "verbose.h": "support",
    "version.h": "include",
    "verify_build.h": "include",
}

# Forward declaration files aren't needed when building dependency graphs. Skip them.
skip_files = [
    "extern_posix.h", "extern_darwin.h", "extern_win.h", "extern.h", 
    "extern_linux.h", "wt_internal.h", "wiredtiger_ext.h"
]
