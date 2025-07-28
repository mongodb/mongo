"""This file contains compiler flags that is specific to Windows C++ compiling and linking."""

# Flags listed in this file is only visible to the bazel build system.
visibility([
    "//bazel/toolchains/cc",
    "//bazel",
])

# https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library?view=msvc-170
#   /MD defines _MT and _DLL and links in MSVCRT.lib into each .obj file
#   /MDd defines _DEBUG, _MT, and _DLL and link MSVCRTD.lib into each .obj file
WINDOWS_MULTITHREAD_RUNTIME_COPTS = select({
    "//bazel/config:windows_dbg_disabled": [
        "/MD",
    ],
    "//bazel/config:windows_dbg_enabled": [
        "/MDd",
    ],
    "//conditions:default": [],
})

# /O1 optimize for size
# /O2 optimize for speed (as opposed to size)
# /Oy- disable frame pointer optimization (overrides /O2, only affects 32-bit)
# /Zo enables optimizations with modifications to make debugging easier
WINDOWS_OPT_COPTS = select({
    # This is opt=debug, not to be confused with (opt=on && dbg=on)
    "//bazel/config:windows_opt_debug": [
        "/Ox",
        "/Zo",
        "/Oy-",
    ],
    "//bazel/config:windows_opt_off": [
        "/Od",
    ],
    "//bazel/config:windows_opt_on": [
        "/O2",
        "/Oy-",
    ],
    "//bazel/config:windows_opt_size": [
        "/Os",
        "/Oy-",
    ],
    "//conditions:default": [],
})

# Enable Stack Frame Run-Time Error Checking; Reports when a variable is used
# without having been initialized (implies /Od: no optimizations)
WINDOWS_RUNTIME_ERROR_CHECK_COPTS = select({
    "//bazel/config:windows_opt_off_dbg_enabled": [
        "/RTC1",
    ],
    "//conditions:default": [],
})

WINDOWS_DEBUG_COPTS = select({
    "//bazel/config:windows_debug_symbols_enabled": [
        # Generate debug info into the object files
        "/Z7",
    ],
    "//conditions:default": [],
})

WINDOWS_DEFAULT_LINKFLAGS = select({
    "//bazel/config:windows_debug_symbols_enabled": [
        # /DEBUG will tell the linker to create a .pdb file which WinDbg and
        # Visual Studio will use to resolve symbols if you want to debug a
        # release-mode image.
        #
        # Note that this means we can't do parallel links in the build.
        #
        # Also note that this has nothing to do with _DEBUG or optimization.

        # If the user set a /DEBUG flag explicitly, don't add another. Otherwise
        # use the standard /DEBUG flag, since we always want PDBs.
        "/DEBUG",
    ],
    "//conditions:default": [],
})

# Windows non optimized builds will cause the PDB to blow up in size, this
# allows a larger PDB. The flag is undocumented at the time of writing but the
# microsoft thread which brought about its creation can be found here:
# https://developercommunity.visualstudio.com/t/pdb-limit-of-4-gib-is-likely-to-be-a-problem-in-a/904784
#
# Without this flag MSVC will report a red herring error message, about disk
# space or invalid path.
WINDOWS_PDB_PAGE_SIZE_LINKOPT = select({
    "//bazel/config:windows_opt_off": [
        "/pdbpagesize:16384",
    ],
    "//conditions:default": [],
})

# Disable incremental link - avoid the level of indirection for function calls
WINDOWS_INCREMENTAL_LINKOPT = select({
    "//bazel/config:windows_opt_any": [
        "/INCREMENTAL:NO",
    ],
    "//conditions:default": [],
})

SASL_WINDOWS_COPTS = select({
    "@platforms//os:windows": ["-Iexternal/windows_sasl/include"],
    "//conditions:default": [],
})

MONGO_WIN_CC_COPTS = (
    WINDOWS_DEBUG_COPTS +
    WINDOWS_OPT_COPTS +
    WINDOWS_RUNTIME_ERROR_CHECK_COPTS +
    SASL_WINDOWS_COPTS
)

MONGO_WIN_CC_LINKFLAGS = (
    WINDOWS_DEFAULT_LINKFLAGS +
    WINDOWS_PDB_PAGE_SIZE_LINKOPT +
    WINDOWS_INCREMENTAL_LINKOPT
)
