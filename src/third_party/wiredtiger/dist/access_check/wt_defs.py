from layercparse import Module

extraFiles = ["src/include/wiredtiger.in"]

modules = [
    # Modules in subdirectories of src/
    Module("block"),
    Module("block_cache", fileAliases=["block_chunkcache"], sourceAliases = ["blkcache", "bm"]),
    Module("bloom"),
    Module("btree", fileAliases=["btmem", "btree_cmp", "dhandle", "modify", "ref", "serial"]),
    Module("call_log"),
    # Module("checksum"),
    Module("conf", sourceAliases=["conf_keys"]),
    Module("config"),
    Module("conn", fileAliases=["connection"], sourceAliases=["connection"]),
    Module("cursor", sourceAliases=["cur", "btcur", "curbackup"]),
    Module("evict", fileAliases=["cache"]),
    Module("history", sourceAliases = ["hs"]),
    Module("log"),
    Module("lsm", sourceAliases=["clsm"]),
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
    Module("txn", sourceAliases=["truncate"]),
    # Module("utilities"),

    # Directory-less modules
    Module("bitstring"),
    Module("cell"),
    Module("checkpoint", sourceAliases=["ckpt"]),
    Module("column", sourceAliases=["col"]),
    Module("compact"),
    Module("generation", sourceAliases=["gen"]),
    Module("pack", fileAliases=["intpack"]),
    Module("stat"),
]

extraMacros = [
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
