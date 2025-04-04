"""This file contains compiler flags that is specific to Windows C++ compiling and linking."""

# Flags listed in this file is only visible to the bazel build system.
visibility("//bazel")

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

WINDOWS_GENERAL_COPTS = select({
    "@platforms//os:windows": [
        # /EHsc exception handling style for visual studio
        "/EHsc",

        # /W3 warning level
        "/W3",

        # Support large object files since some unit-test sources contain a lot
        # of code
        "/bigobj",

        # Set Source and Executable character sets to UTF-8, this will produce a
        # warning C4828 if the file contains invalid UTF-8.
        "/utf-8",

        # Specify standards conformance mode to the compiler.
        "/permissive-",

        # Enables the __cplusplus preprocessor macro to report an updated value
        # for recent C++ language standards support.
        "/Zc:__cplusplus",

        # Tells the compiler to preferentially call global operator delete or
        # operator delete[] functions that have a second parameter of type
        # size_t when the size of the object is available.
        "/Zc:sizedDealloc",

        # Treat volatile according to the ISO standard and do not guarantee
        # acquire/release semantics.
        "/volatile:iso",

        # Tell CL to produce more useful error messages.
        "/diagnostics:caret",

        # Don't send error reports in case of internal compiler error
        "/errorReport:none",
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

# Suppress some warnings we don't like, or find necessary to
# suppress. Please keep this list alphabetized and commented.
WINDOWS_SUPRESSED_WARNINGS_COPTS = select({
    "@platforms//os:windows": [
        # C4068: unknown pragma. added so that we can specify unknown
        # pragmas for other compilers.
        "/wd4068",

        # C4244: 'conversion' conversion from 'type1' to 'type2', possible loss
        # of data. An integer type is converted to a smaller integer type.
        "/wd4244",

        # C4267: 'var' : conversion from 'size_t' to 'type', possible loss of
        # data. When compiling with /Wp64, or when compiling on a 64-bit
        # operating system, type is 32 bits but size_t is 64 bits when compiling
        # for 64-bit targets. To fix this warning, use size_t instead of a type.
        "/wd4267",

        # C4290: C++ exception specification ignored except to indicate a
        # function is not __declspec(nothrow). A function is declared using
        # exception specification, which Visual C++ accepts but does not
        # implement.
        "/wd4290",

        # C4351: On extremely old versions of MSVC (pre 2k5), default
        # constructing an array member in a constructor's initialization list
        # would not zero the array members "in some cases". Since we don't
        # target MSVC versions that old, this warning is safe to ignore.
        "/wd4351",

        # C4355: 'this' : used in base member initializer list. The this pointer
        # is valid only within nonstatic member functions. It cannot be used in
        # the initializer list for a base class.
        "/wd4355",

        # C4373: Older versions of MSVC would fail to make a function in a
        # derived class override a virtual function in the parent, when defined
        # inline and at least one of the parameters is made const. The behavior
        # is incorrect under the standard. MSVC is fixed now, and the warning
        # exists merely to alert users who may have relied upon the older,
        # non-compliant behavior. Our code should not have any problems with the
        # older behavior, so we can just disable this warning.
        "/wd4373",

        # C4800: 'type' : forcing value to bool 'true' or 'false' (performance
        # warning). This warning is generated when a value that is not bool is
        # assigned or coerced into type bool.
        "/wd4800",

        # C4251: This warning attempts to prevent usage of CRT (C++ standard
        # library) types in DLL interfaces. That is a good idea for DLLs you
        # ship to others, but in our case, we know that all DLLs are built
        # consistently. Suppress the warning.
        "/wd4251",

        # mozjs requires the following
        #  'declaration' : no matching operator delete found; memory will not be
        #                  freed if initialization throws an exception
        "/wd4291",
    ],
    "//conditions:default": [],
})

WINDOWS_WARNINGS_AS_ERRORS_COPTS = select({
    "@platforms//os:windows": [
        # some warnings we should treat as errors:
        # c4013
        #  'function' undefined; assuming extern returning int
        #
        # This warning occurs when files compiled for the C language use
        # functions not defined in a header file.
        "/we4013",

        # c4099
        #  'identifier' : type name first seen using 'objecttype1' now seen
        #                 using 'objecttype2'
        #
        # This warning occurs when classes and structs are declared with a mix
        # of struct and classwhich can cause linker failures
        "/we4099",

        # c4930
        #  'identifier': prototyped function not called (was a variable
        #                definition intended?)
        #
        # This warning indicates a most-vexing parse error, where a user
        # declared a function that was probably intended as a variable
        # definition. A common example is accidentally declaring a function
        # called lock that takes a mutex when one meant to create a guard object
        # called lock on the stack.
        "/we4930",
    ],
    "//conditions:default": [],
})

MSVC_OPT_COPTS = select({
    "//bazel/config:msvc_opt": [
        # https://devblogs.microsoft.com/cppblog/introducing-gw-compiler-switch/
        "/Gw",
        "/Gy",

        # https://devblogs.microsoft.com/cppblog/linker-enhancements-in-visual-studio-2013-update-2-ctp2/
        "/Zc:inline",
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

# This gives 32-bit programs 4 GB of user address space in WOW64, ignored in
# 64-bit builds.
WINDOWS_LARGE_ADDRESS_AWARE_LINKFLAG = select({
    "@platforms//os:windows": [
        "/LARGEADDRESSAWARE",
    ],
    "//conditions:default": [],
})

MSVC_OPT_LINKFLAGS = select({
    "//bazel/config:msvc_opt": [
        # https://devblogs.microsoft.com/cppblog/introducing-gw-compiler-switch/
        "/OPT:REF",
    ],
    "//conditions:default": [],
})

SASL_WINDOWS_COPTS = select({
    "@platforms//os:windows": ["-Iexternal/windows_sasl/include"],
    "//conditions:default": [],
})

SASL_WINDOWS_LINKFLAGS = select({
    "@platforms//os:windows": ["/LIBPATH:external/windows_sasl/lib"],
    "//conditions:default": [],
})

GLOBAL_WINDOWS_LIBRAY_LINKFLAGS = select({
    "@platforms//os:windows": [
        "bcrypt.lib",
        "Dnsapi.lib",
        "Crypt32.lib",
        "Version.lib",
        "Winmm.lib",
        "Iphlpapi.lib",
        "Pdh.lib",
        "kernel32.lib",
        "shell32.lib",
        "ws2_32.lib",
        "DbgHelp.lib",
        "Psapi.lib",
        "Secur32.lib",
    ],
    "//conditions:default": [],
})

MONGO_WIN_CC_COPTS = (
    WINDOWS_GENERAL_COPTS +
    WINDOWS_DEBUG_COPTS +
    WINDOWS_OPT_COPTS +
    WINDOWS_RUNTIME_ERROR_CHECK_COPTS +
    WINDOWS_SUPRESSED_WARNINGS_COPTS +
    WINDOWS_WARNINGS_AS_ERRORS_COPTS +
    MSVC_OPT_COPTS +
    SASL_WINDOWS_COPTS
)

MONGO_WIN_CC_LINKFLAGS = (
    WINDOWS_DEFAULT_LINKFLAGS +
    WINDOWS_PDB_PAGE_SIZE_LINKOPT +
    WINDOWS_INCREMENTAL_LINKOPT +
    WINDOWS_LARGE_ADDRESS_AWARE_LINKFLAG +
    MSVC_OPT_LINKFLAGS +
    SASL_WINDOWS_LINKFLAGS +
    GLOBAL_WINDOWS_LIBRAY_LINKFLAGS
)
