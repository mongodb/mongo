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

{
    "extraFiles": ["src/include/wiredtiger.h.in"],
    "modules": [
        # Modules in subdirectories of src/
        Module("block"),
        Module("block_cache",
               sourceAliases = ["blkcache", "bm"]),
        Module("btree", fileAliases=["btmem", "btree_cmp", "dhandle", "modify", "ref", "serial"],
               sourceAliases = ["ref", "page", "dhandle", "btcur"]),
        Module("call_log"),
        # Module("checksum"),
        Module("cache"),
        Module("checkpoint", sourceAliases=["ckpt"]),
        Module("conf", sourceAliases=["conf_keys"]),
        Module("config"),
        Module("conn", fileAliases=["connection"], sourceAliases=["connection"]),
        Module("cursor", sourceAliases=["cur", "curbackup"]),
        Module("evict"),
        Module("history", sourceAliases = ["hs"]),
        Module("live_restore"),
        Module("log"),
        Module("meta", sourceAliases=["metadata"]),
        Module("optrack"),
        # Module("os", fileAliases = ["os_common", "os_darwin", "os_linux", "os_posix", "os_win"]),
        Module("packing", sourceAliases=["pack"]),
        Module("reconcile", sourceAliases = ["rec"]),
        Module("rollback_to_stable", sourceAliases = ["rts"]),
        Module("schema"),
        Module("session"),
        # Module("support"),
        Module("tiered"),
        Module("truncate"),
        Module("txn"),
        # Module("utilities"),

        # Directory-less modules
        Module("bitstring"),
        Module("cell"),
        Module("column", sourceAliases=["col"]),
        Module("compact"),
        Module("generation", sourceAliases=["gen"]),
        Module("pack", fileAliases=["intpack"]),
        Module("stat"),
    ],
    "extraMacros": [
        {"name": "__attribute__", "args": 1},
        {"name": "WT_UNUSED",     "args": 1},

        {"name": "WT_ATTRIBUTE_LIBRARY_VISIBLE"},
        {"name": "WT_INLINE"},
        {"name": "inline"},

        {"name": "WT_COMPILER_BARRIER",  "args": ("__VA_ARGS__"),
            "body": "WT_COMPILER_BARRIER", "is_va_args": True},
        {"name": "WT_FULL_BARRIER",      "args": ("__VA_ARGS__"),
            "body": "WT_FULL_BARRIER", "is_va_args": True},
        {"name": "WT_PAUSE",             "args": ("__VA_ARGS__"),
            "body": "WT_PAUSE", "is_va_args": True},
        {"name": "WT_ACQUIRE_BARRIER",   "args": ("__VA_ARGS__"),
            "body": "WT_ACQUIRE_BARRIER", "is_va_args": True},
        {"name": "WT_RELEASE_BARRIER",   "args": ("__VA_ARGS__"),
            "body": "WT_RELEASE_BARRIER", "is_va_args": True},
    ]
}
