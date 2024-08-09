# Common mongo-specific bazel build rules intended to be used in individual BUILD files in the "src/" subtree.
load("@poetry//:dependencies.bzl", "dependency")
load("//bazel:separate_debug.bzl", "CC_SHARED_LIBRARY_SUFFIX", "SHARED_ARCHIVE_SUFFIX", "WITH_DEBUG_SUFFIX", "extract_debuginfo", "extract_debuginfo_binary")

# https://learn.microsoft.com/en-us/cpp/build/reference/md-mt-ld-use-run-time-library?view=msvc-170
#   /MD defines _MT and _DLL and links in MSVCRT.lib into each .obj file
#   /MDd defines _DEBUG, _MT, and _DLL and link MSVCRTD.lib into each .obj file
WINDOWS_MULTITHREAD_RUNTIME_COPTS = select({
    "//bazel/config:windows_dbg_enabled": [
        "/MDd",
    ],
    "//bazel/config:windows_dbg_disabled": [
        "/MD",
    ],
    "//conditions:default": [],
})

# /O1 optimize for size
# /O2 optimize for speed (as opposed to size)
# /Oy- disable frame pointer optimization (overrides /O2, only affects 32-bit)
# /Zo enables optimizations with modifications to make debugging easier
WINDOWS_OPT_COPTS = select({
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
    # This is opt=debug, not to be confused with (opt=on && dbg=on)
    "//bazel/config:windows_opt_debug": [
        "/Ox",
        "/Zo",
        "/Oy-",
    ],
    "//conditions:default": [],
})

# Enable Stack Frame Run-Time Error Checking; Reports when a variable is used without having been initialized (implies /Od: no optimizations)
WINDOWS_RUNTIME_ERROR_CHECK_COPTS = select({
    "//bazel/config:windows_opt_off_dbg_enabled": [
        "/RTC1",
    ],
    "//conditions:default": [],
})

WINDOWS_GENERAL_COPTS = select({
    "@platforms//os:windows": [
        # /EHsc exception handling style for visual studio
        # /W3 warning level
        "/EHsc",
        "/W3",

        # Support large object files since some unit-test sources contain a lot of code
        "/bigobj",

        # Set Source and Executable character sets to UTF-8, this will produce a warning C4828 if the
        # file contains invalid UTF-8.
        "/utf-8",

        # Specify standards conformance mode to the compiler.
        "/permissive-",

        # Enables the __cplusplus preprocessor macro to report an updated value for recent C++ language
        # standards support.
        "/Zc:__cplusplus",

        # Tells the compiler to preferentially call global operator delete or operator delete[]
        # functions that have a second parameter of type size_t when the size of the object is available.
        "/Zc:sizedDealloc",

        # Treat volatile according to the ISO standard and do not guarantee acquire/release semantics.
        "/volatile:iso",

        # Tell CL to produce more useful error messages.
        "/diagnostics:caret",

        # Don't send error reports in case of internal compiler error
        "/errorReport:none",

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

        # C4244: 'conversion' conversion from 'type1' to 'type2',
        # possible loss of data. An integer type is converted to a
        # smaller integer type.
        "/wd4244",

        # C4267: 'var' : conversion from 'size_t' to 'type', possible
        # loss of data. When compiling with /Wp64, or when compiling
        # on a 64-bit operating system, type is 32 bits but size_t is
        # 64 bits when compiling for 64-bit targets. To fix this
        # warning, use size_t instead of a type.
        "/wd4267",

        # C4290: C++ exception specification ignored except to
        # indicate a function is not __declspec(nothrow). A function
        # is declared using exception specification, which Visual C++
        # accepts but does not implement.
        "/wd4290",

        # C4351: On extremely old versions of MSVC (pre 2k5), default
        # constructing an array member in a constructor's
        # initialization list would not zero the array members "in
        # some cases". Since we don't target MSVC versions that old,
        # this warning is safe to ignore.
        "/wd4351",

        # C4355: 'this' : used in base member initializer list. The
        # this pointer is valid only within nonstatic member
        # functions. It cannot be used in the initializer list for a
        # base class.
        "/wd4355",

        # C4373: Older versions of MSVC would fail to make a function
        # in a derived class override a virtual function in the
        # parent, when defined inline and at least one of the
        # parameters is made const. The behavior is incorrect under
        # the standard. MSVC is fixed now, and the warning exists
        # merely to alert users who may have relied upon the older,
        # non-compliant behavior. Our code should not have any
        # problems with the older behavior, so we can just disable
        # this warning.
        "/wd4373",

        # C4800: 'type' : forcing value to bool 'true' or 'false'
        # (performance warning). This warning is generated when a
        # value that is not bool is assigned or coerced into type
        # bool.
        "/wd4800",

        # C4251: This warning attempts to prevent usage of CRT (C++
        # standard library) types in DLL interfaces. That is a good
        # idea for DLLs you ship to others, but in our case, we know
        # that all DLLs are built consistently. Suppress the warning.
        "/wd4251",

        # mozjs requires the following
        #  'declaration' : no matching operator delete found; memory will not be freed if
        #  initialization throws an exception
        "/wd4291",
    ],
    "//conditions:default": [],
})

WINDOWS_WARNINGS_AS_ERRORS_COPTS = select({
    "@platforms//os:windows": [
        # some warnings we should treat as errors:
        # c4013
        #  'function' undefined; assuming extern returning int
        #    This warning occurs when files compiled for the C language use functions not defined
        #    in a header file.
        "/we4013",

        # c4099
        #  'identifier' : type name first seen using 'objecttype1' now seen using 'objecttype2'
        #    This warning occurs when classes and structs are declared with a mix of struct and class
        #    which can cause linker failures
        "/we4099",

        # c4930
        #  'identifier': prototyped function not called (was a variable definition intended?)
        #     This warning indicates a most-vexing parse error, where a user declared a function that
        #     was probably intended as a variable definition.  A common example is accidentally
        #     declaring a function called lock that takes a mutex when one meant to create a guard
        #     object called lock on the stack.
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

WINDOWS_COPTS = WINDOWS_GENERAL_COPTS + WINDOWS_OPT_COPTS + WINDOWS_MULTITHREAD_RUNTIME_COPTS + WINDOWS_RUNTIME_ERROR_CHECK_COPTS + \
                WINDOWS_SUPRESSED_WARNINGS_COPTS + WINDOWS_WARNINGS_AS_ERRORS_COPTS + MSVC_OPT_COPTS

WINDOWS_DEFAULT_LINKFLAGS = select({
    "@platforms//os:windows": [
        # /DEBUG will tell the linker to create a .pdb file
        # which WinDbg and Visual Studio will use to resolve
        # symbols if you want to debug a release-mode image.
        # Note that this means we can't do parallel links in the build.
        #
        # Please also note that this has nothing to do with _DEBUG or optimization.

        # If the user set a /DEBUG flag explicitly, don't add
        # another. Otherwise use the standard /DEBUG flag, since we always
        # want PDBs.
        "/DEBUG",
    ],
    "//conditions:default": [],
})

# Windows non optimized builds will cause the PDB to blow up in size,
# this allows a larger PDB. The flag is undocumented at the time of writing
# but the microsoft thread which brought about its creation can be found here:
# https://developercommunity.visualstudio.com/t/pdb-limit-of-4-gib-is-likely-to-be-a-problem-in-a/904784
#
# Without this flag MSVC will report a red herring error message, about disk space or invalid path.
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

# This gives 32-bit programs 4 GB of user address space in WOW64, ignored in 64-bit builds.
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

WINDOWS_LINKFLAGS = WINDOWS_DEFAULT_LINKFLAGS + WINDOWS_PDB_PAGE_SIZE_LINKOPT + WINDOWS_INCREMENTAL_LINKOPT + \
                    WINDOWS_LARGE_ADDRESS_AWARE_LINKFLAG + MSVC_OPT_LINKFLAGS

WINDOWS_DEFINES = select({
    "@platforms//os:windows": [
        # This tells the Windows compiler not to link against the .lib files
        # and to use boost as a bunch of header-only libraries
        "BOOST_ALL_NO_LIB",
        "_UNICODE",
        "UNICODE",

        # Temporary fixes to allow compilation with VS2017
        "_SILENCE_CXX17_ALLOCATOR_VOID_DEPRECATION_WARNING",
        "_SILENCE_CXX17_OLD_ALLOCATOR_MEMBERS_DEPRECATION_WARNING",
        "_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING",

        # TODO(SERVER-60151): Until we are fully in C++20 mode, it is
        # easier to simply suppress C++20 deprecations. After we have
        # switched over we should address any actual deprecated usages
        # and then remove this flag.
        "_SILENCE_ALL_CXX20_DEPRECATION_WARNINGS",
        "_CONSOLE",
        "_CRT_SECURE_NO_WARNINGS",
        "_ENABLE_EXTENDED_ALIGNED_STORAGE",
        "_SCL_SECURE_NO_WARNINGS",
    ],
    "//conditions:default": [],
})

LINUX_DEFINES = select({
    "@platforms//os:linux": [
        # On linux, C code compiled with gcc/clang -std=c11 causes
        # __STRICT_ANSI__ to be set, and that drops out all of the feature
        # test definitions, resulting in confusing errors when we run C
        # language configure checks and expect to be able to find newer
        # POSIX things. Explicitly enabling _XOPEN_SOURCE fixes that, and
        # should be mostly harmless as on Linux, these macros are
        # cumulative. The C++ compiler already sets _XOPEN_SOURCE, and,
        # notably, setting it again does not disable any other feature
        # test macros, so this is safe to do. Other platforms like macOS
        # and BSD have crazy rules, so don't try this there.
        #
        # Furthermore, as both C++ compilers appear to define _GNU_SOURCE
        # unconditionally (because libstdc++ requires it), it seems
        # prudent to explicitly add that too, so that C language checks
        # see a consistent set of definitions.
        "_XOPEN_SOURCE=700",
        "_GNU_SOURCE",
    ],
    "//conditions:default": [],
})

MACOS_DEFINES = select({
    "@platforms//os:macos": [
        # TODO SERVER-54659 - ASIO depends on std::result_of which was removed in C++ 20
        # xcode15 does not have backwards compatibility
        "ASIO_HAS_STD_INVOKE_RESULT",
        # This is needed to compile boost on the newer xcodes
        "BOOST_NO_CXX98_FUNCTION_BASE",
    ],
    "//conditions:default": [],
})

ABSEIL_DEFINES = [
    "ABSL_FORCE_ALIGNED_ACCESS",
]

BOOST_DEFINES = [
    "BOOST_ENABLE_ASSERT_DEBUG_HANDLER",
    # TODO: Ideally, we could not set this define in C++20
    # builds, but at least our current Xcode 12 doesn't offer
    # std::atomic_ref, so we cannot.
    "BOOST_FILESYSTEM_NO_CXX20_ATOMIC_REF",
    "BOOST_LOG_NO_SHORTHAND_NAMES",
    "BOOST_LOG_USE_NATIVE_SYSLOG",
    "BOOST_LOG_WITHOUT_THREAD_ATTR",
    "BOOST_MATH_NO_LONG_DOUBLE_MATH_FUNCTIONS",
    "BOOST_SYSTEM_NO_DEPRECATED",
    "BOOST_THREAD_USES_DATETIME",
    "BOOST_THREAD_VERSION=5",
] + select({
    "//bazel/config:linkdynamic_not_shared_archive": ["BOOST_LOG_DYN_LINK"],
    "//conditions:default": [],
}) + select({
    "@platforms//os:windows": ["BOOST_ALL_NO_LIB"],
    "//conditions:default": [],
})

# Fortify only possibly makes sense on POSIX systems, and we know that clang is not a valid
# combination: http://lists.llvm.org/pipermail/cfe-dev/2015-November/045852.html
GCC_OPT_DEFINES = select({
    "//bazel/config:gcc_opt": ["_FORTIFY_SOURCE=2"],
    "//conditions:default": [],
})

LINUX_OPT_COPTS = select({
    "//bazel/config:linux_opt_off": [
        "-O0",
    ],
    "//bazel/config:linux_opt_on": [
        "-O2",
    ],
    "//bazel/config:linux_opt_size": [
        "-Os",
    ],
    # This is opt=debug, not to be confused with (opt=on && dbg=on)
    "//bazel/config:linux_opt_debug": [
        "-Og",
    ],
    "//conditions:default": [],
})

GCC_OR_CLANG_WARNINGS_COPTS = select({
    "//bazel/config:gcc_or_clang": [
        # Enable all warnings by default.
        "-Wall",

        # Warn on comparison between signed and unsigned integer expressions.
        "-Wsign-compare",

        # Do not warn on unknown pragmas.
        "-Wno-unknown-pragmas",

        # Warn if a precompiled header (see Precompiled Headers) is found in the search path but can't be used.
        "-Winvalid-pch",

        # Warn when hiding a virtual function.
        "-Woverloaded-virtual",

        # This warning was added in g++-4.8.
        "-Wno-unused-local-typedefs",

        # Clang likes to warn about unused functions, which seems a tad aggressive and breaks
        # -Werror, which we want to be able to use.
        "-Wno-unused-function",

        # TODO: Note that the following two flags are added to CCFLAGS even though they are
        # really C++ specific. We need to do this because SCons passes CXXFLAGS *before*
        # CCFLAGS, but CCFLAGS contains -Wall, which re-enables the warnings we are trying to
        # suppress. In the future, we should move all warning flags to CCWARNFLAGS and
        # CXXWARNFLAGS and add these to CCOM and CXXCOM as appropriate.
        #
        # Clang likes to warn about unused private fields, but some of our third_party
        # libraries have such things.
        "-Wno-unused-private-field",

        # Prevents warning about using deprecated features (such as auto_ptr in c++11)
        # Using -Wno-error=deprecated-declarations does not seem to work on some compilers,
        # including at least g++-4.6.
        "-Wno-deprecated-declarations",

        # As of clang-3.4, this warning appears in v8, and gets escalated to an error.
        "-Wno-tautological-constant-out-of-range-compare",

        # As of clang in Android NDK 17, these warnings appears in boost and/or ICU, and get escalated to errors
        "-Wno-tautological-constant-compare",
        "-Wno-tautological-unsigned-zero-compare",
        "-Wno-tautological-unsigned-enum-zero-compare",

        # New in clang-3.4, trips up things mostly in third_party, but in a few places in the
        # primary mongo sources as well.
        "-Wno-unused-const-variable",

        # This has been suppressed in gcc 4.8, due to false positives, but not in clang.  So
        # we explicitly disable it here.
        "-Wno-missing-braces",

        # Suppress warnings about not consistently using override everywhere in a class. It seems
        # very pedantic, and we have a fair number of instances.
        "-Wno-inconsistent-missing-override",

        # Don't issue warnings about potentially evaluated expressions
        "-Wno-potentially-evaluated-expression",

        # SERVER-76472 we don't try to maintain ABI so disable warnings about possible ABI issues.
        "-Wno-psabi",

        # Warn about moves of prvalues, which can inhibit copy elision.
        "-Wpessimizing-move",

        # Disable warning about templates that can't be implicitly instantiated. It is an attempt to
        # make a link error into an easier-to-debug compiler failure, but it triggers false
        # positives if explicit instantiation is used in a TU that can see the full definition. This
        # is a problem at least for the S2 headers.
        "-Wno-undefined-var-template",

        # This warning was added in clang-4.0, but it warns about code that is required on some
        # platforms. Since the warning just states that 'explicit instantiation of [a template] that
        # occurs after an explicit specialization has no effect', it is harmless on platforms where
        # it isn't required
        "-Wno-instantiation-after-specialization",

        # This warning was added in clang-5 and flags many of our lambdas. Since it isn't actively
        # harmful to capture unused variables we are suppressing for now with a plan to fix later.
        "-Wno-unused-lambda-capture",

        # This warning was added in Apple clang version 11 and flags many explicitly defaulted move
        # constructors and assignment operators for being implicitly deleted, which is not useful.
        "-Wno-defaulted-function-deleted",
    ],
    "//conditions:default": [],
})

CLANG_WARNINGS_COPTS = select({
    "//bazel/config:compiler_type_clang": [
        # SERVER-44856: Our windows builds complain about unused
        # exception parameters, but GCC and clang don't seem to do
        # that for us automatically. In the interest of making it more
        # likely to catch these errors early, add the (currently clang
        # only) flag that turns it on.
        "-Wunused-exception-parameter",
    ],
    "//conditions:default": [],
})

CLANG_FNO_LIMIT_DEBUG_INFO = select({
    "//bazel/config:compiler_type_clang": [
        # We add this flag to make clang emit debug info for c++ stl types so that our pretty
        # printers will work with newer clang's which omit this debug info. This does increase
        # the overall debug info size.
        "-fno-limit-debug-info",
    ],
    "//conditions:default": [],
})

MACOS_WARNINGS_COPTS = select({
    "@platforms//os:macos": [
        # As of XCode 9, this flag must be present (it is not enabled
        # by -Wall), in order to enforce that -mXXX-version-min=YYY
        # will enforce that you don't use APIs from ZZZ.
        "-Wunguarded-availability",
        "-Wno-enum-constexpr-conversion",
    ],
    "//conditions:default": [],
})

GCC_OR_CLANG_GENERAL_COPTS = select({
    "//bazel/config:gcc_or_clang": [
        # Generate unwind table in DWARF format, if supported by target machine.
        # The table is exact at each instruction boundary, so it can be used for stack unwinding
        # from asynchronous events (such as debugger or garbage collector).
        "-fasynchronous-unwind-tables",

        # For debug builds with tcmalloc, we need the frame pointer so it can
        # record the stack of allocations.
        # We also need the stack pointer for stack traces unless libunwind is enabled.
        # Enable frame pointers by default.
        "-fno-omit-frame-pointer",

        # Enable strong by default, this may need to be softened to -fstack-protector-all if
        # we run into compatibility issues.
        "-fstack-protector-strong",
    ],
    "//conditions:default": [],
})

LINUX_PTHREAD_LINKFLAG = select({
    "@platforms//os:linux": [
        # Adds support for multithreading with the pthreads library.
        # This option sets flags for both the preprocessor and linker.
        "-pthread",
    ],
    "//conditions:default": [],
})

RDYNAMIC_LINKFLAG = select({
    # We need to use rdynamic for backtraces with glibc unless we have libunwind.
    "@platforms//os:linux": [
        # Pass the flag -export-dynamic to the ELF linker, on targets that support it.
        # This instructs the linker to add all symbols, not only used ones, to the dynamic symbol table.
        # This option is needed for some uses of dlopen or to allow obtaining backtraces from within a program.
        "-rdynamic",
    ],
    "//conditions:default": [],
})

# These are added to both copts and linker flags.
DWARF_VERSION_FEATURES = select({
    "//bazel/config:dwarf_version_4_linux": [
        "dwarf-4",
    ],
    "//bazel/config:dwarf_version_5_linux": [
        "dwarf-5",
    ],
    "//conditions:default": [],
})

# SERVER-9761: Ensure early detection of missing symbols in dependent
# libraries at program startup. For non-release dynamic builds we disable
# this behavior in the interest of improved mongod startup times.
# Xcode15 removed bind_at_load functionality so we cannot have a selection for macosx here
# ld: warning: -bind_at_load is deprecated on macOS
# TODO: SERVER-90596 reenable loading at startup
BIND_AT_LOAD_LINKFLAGS = select({
    "//bazel/config:linkstatic_enabled_linux": [
        "-Wl,-z,now",
    ],
    "//conditions:default": [],
})

# Disable floating-point contractions such as forming of fused multiply-add operations.
FLOATING_POINT_COPTS = select({
    "//bazel/config:compiler_type_clang": ["-ffp-contract=off"],
    "//bazel/config:compiler_type_gcc": ["-ffp-contract=off"],

    # msvc defaults to /fp:precise. Visual Studio 2022 does not emit floating-point contractions
    # with /fp:precise, but previous versions can. Disable contractions altogether by using
    # /fp:strict.
    "//bazel/config:compiler_type_msvc": ["/fp:strict"],
})

IMPLICIT_FALLTHROUGH_COPTS = select({
    "//bazel/config:compiler_type_clang": ["-Wimplicit-fallthrough"],
    "//bazel/config:compiler_type_gcc": ["-Wimplicit-fallthrough=5"],
    "//conditions:default": [],
})

EXTRA_GLOBAL_LIBS_LINKFLAGS = select({
    "@platforms//os:linux": [
        "-lm",
        "-lresolv",
    ],
    "@platforms//os:macos": [
        "-lresolv",
    ],
    "//conditions:default": [],
})

# TODO SERVER-85340 fix this error message when libc++ is readded to the toolchain
LIBCXX_ERROR_MESSAGE = (
    "\nError:\n" +
    "    libc++ is not currently supported in the mongo toolchain.\n" +
    "    Follow this ticket to see when support is being added SERVER-85340\n" +
    "    We currently only support passing the libcxx config on macos for compatibility reasons.\n" +
    "    libc++ requires these configuration:\n" +
    "    --//bazel/config:compiler_type=clang\n"
)

LIBCXX_COPTS = select({
    ("//bazel/config:use_libcxx_required_settings"): ["-stdlib=libc++"],
    ("//bazel/config:use_libcxx_disabled"): [],
}, no_match_error = LIBCXX_ERROR_MESSAGE)

LIBCXX_LINKFLAGS = LIBCXX_COPTS

# TODO SERVER-54659 - ASIO depends on std::result_of which was removed in C++ 20
LIBCXX_DEFINES = select({
    ("//bazel/config:use_libcxx_required_settings"): ["ASIO_HAS_STD_INVOKE_RESULT"],
    ("//bazel/config:use_libcxx_disabled"): [],
}, no_match_error = LIBCXX_ERROR_MESSAGE)

DEBUG_DEFINES = select({
    "//bazel/config:dbg_enabled": [],
    "//conditions:default": ["NDEBUG"],
})

PCRE2_DEFINES = ["PCRE2_STATIC"]

SAFEINT_DEFINES = ["SAFEINT_USE_INTRINSICS=0"]

LINKER_ERROR_MESSAGE = (
    "\nError:\n" +
    "    --//bazel/config:linker=lld is not supported on s390x"
)

LINKER_LINKFLAGS = select({
    "//bazel/config:linker_default": [],
    "//bazel/config:linker_gold": ["-fuse-ld=gold"],
    "//bazel/config:linker_lld_valid_settings": ["-fuse-ld=lld"],
}, no_match_error = LINKER_ERROR_MESSAGE)

REQUIRED_SETTINGS_LIBUNWIND_ERROR_MESSAGE = (
    "\nError:\n" +
    "    libunwind=on is only supported on linux"
)

# These will throw an error if the following condition is not met:
# (libunwind == on && os == linux) || libunwind == off || libunwind == auto
LIBUNWIND_DEPS = select({
    "//bazel/config:libunwind_enabled": ["//src/third_party/unwind:unwind"],
    "//bazel/config:_libunwind_off": [],
    "//bazel/config:_libunwind_disabled_by_auto": [],
}, no_match_error = REQUIRED_SETTINGS_LIBUNWIND_ERROR_MESSAGE)

REQUIRED_SETTINGS_SANITIZER_ERROR_MESSAGE = (
    "\nError:\n" +
    "    any sanitizer requires these configurations:\n" +
    "    --//bazel/config:compiler_type=clang\n" +
    "    --//bazel/config:opt=on [OR] --//bazel/config:opt=debug"
)

# -fno-omit-frame-pointer should be added if any sanitizer flag is used by user
ANY_SANITIZER_AVAILABLE_COPTS = select(
    {
        "//bazel/config:no_enabled_sanitizer": [],
        "//bazel/config:any_sanitizer_required_setting": ["-fno-omit-frame-pointer"],
    },
    no_match_error = REQUIRED_SETTINGS_SANITIZER_ERROR_MESSAGE,
)

ANY_SANITIZER_AVAILABLE_LINKFLAGS = select(
    {
        # Sanitizer libs may inject undefined refs (for hooks) at link time, but
        # the symbols will be available at runtime via the compiler runtime lib.
        "//bazel/config:any_sanitizer_required_setting": ["-Wl,--allow-shlib-undefined"],
        "//bazel/config:no_enabled_sanitizer": [],
    },
    no_match_error = REQUIRED_SETTINGS_SANITIZER_ERROR_MESSAGE,
)

ANY_SANITIZER_GCC_LINKFLAGS = select({
    # GCC's implementation of ASAN depends on libdl.
    "//bazel/config:any_sanitizer_gcc": ["-ldl"],
    "//conditions:default": [],
})

SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE = (
    "\nError:\n" +
    "    address and memory sanitizers require these configurations:\n" +
    "    --//bazel/config:allocator=system\n"
)

ADDRESS_SANITIZER_COPTS = select(
    {
        "//bazel/config:sanitize_address_required_settings": [
            "-fsanitize=address",
            "-fsanitize-blacklist=$(location //etc:asan_denylist_h)",
        ],
        "//bazel/config:asan_disabled": [],
    },
    no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE,
)

ADDRESS_SANITIZER_LINKFLAGS = select(
    {
        "//bazel/config:sanitize_address_required_settings": ["-fsanitize=address"],
        "//bazel/config:asan_disabled": [],
    },
    no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE,
)

# Unfortunately, abseil requires that we make these macros
# (this, and THREAD_ and UNDEFINED_BEHAVIOR_ below) set,
# because apparently it is too hard to query the running
# compiler. We do this unconditionally because abseil is
# basically pervasive via the 'base' library.
ADDRESS_SANITIZER_DEFINES = select(
    {
        ("//bazel/config:sanitize_address_required_settings"): ["ADDRESS_SANITIZER"],
        "//bazel/config:asan_disabled": [],
    },
    no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE,
)

# Makes it easier to debug memory failures at the cost of some perf: -fsanitize-memory-track-origins
MEMORY_SANITIZER_COPTS = select(
    {
        "//bazel/config:sanitize_memory_required_settings": [
            "-fsanitize=memory",
            "-fsanitize-memory-track-origins",
            "-fsanitize-blacklist=$(location //etc:msan_denylist_h)",
        ],
        "//bazel/config:msan_disabled": [],
    },
    no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE,
)

# Makes it easier to debug memory failures at the cost of some perf: -fsanitize-memory-track-origins
MEMORY_SANITIZER_LINKFLAGS = select(
    {
        ("//bazel/config:sanitize_memory_required_settings"): ["-fsanitize=memory"],
        ("//bazel/config:msan_disabled"): [],
    },
    no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE,
)

GENERIC_SANITIZER_ERROR_MESSAGE = (
    "Failed to enable sanitizers with flag: "
)

# We can't include the fuzzer flag with the other sanitize flags
# The libfuzzer library already has a main function, which will cause the dependencies check
# to fail
FUZZER_SANITIZER_COPTS = select(
    {
        ("//bazel/config:sanitize_fuzzer_required_settings"): ["-fsanitize=fuzzer-no-link", "-fprofile-instr-generate", "-fcoverage-mapping"],
        ("//bazel/config:fsan_disabled"): [],
    },
    no_match_error = GENERIC_SANITIZER_ERROR_MESSAGE + "fuzzer",
)

# These flags are needed to generate a coverage report
FUZZER_SANITIZER_LINKFLAGS = select(
    {
        ("//bazel/config:sanitize_fuzzer_required_settings"): ["-fsanitize=fuzzer-no-link", "-fprofile-instr-generate", "-fcoverage-mapping"],
        ("//bazel/config:fsan_disabled"): [],
    },
    no_match_error = GENERIC_SANITIZER_ERROR_MESSAGE + "fuzzer",
)

# Combines following two conditions -
# 1.
# TODO: SERVER-48622
#
# See https://github.com/google/sanitizers/issues/943
# for why we disallow combining TSAN with
# libunwind. We could, atlernatively, have added logic
# to automate the decision about whether to enable
# libunwind based on whether TSAN is enabled, but that
# logic is already complex, and it feels better to
# make it explicit that using TSAN means you won't get
# the benefits of libunwind.
# 2.
# We add supressions based on the library file in etc/tsan.suppressions
# so the link-model needs to be dynamic.

THREAD_SANITIZER_ERROR_MESSAGE = (
    "\nError:\n" +
    "    Build failed due to either -\n" +
    "    Cannot use libunwind with TSAN, please add --//bazel/config:use_libunwind=False to your compile flags or\n" +
    "    TSAN is only supported with dynamic link models, please add --//bazel/config:linkstatic=False to your compile flags.\n"
)

THREAD_SANITIZER_COPTS = select({
    "//bazel/config:sanitize_thread_required_settings": [
        "-fsanitize=thread",
        "-fsanitize-blacklist=$(location //etc:tsan_denylist_h)",
    ],
    "//bazel/config:tsan_disabled": [],
}, no_match_error = THREAD_SANITIZER_ERROR_MESSAGE)

THREAD_SANITIZER_LINKFLAGS = select({
    ("//bazel/config:sanitize_thread_required_settings"): ["-fsanitize=thread"],
    ("//bazel/config:tsan_disabled"): [],
}, no_match_error = THREAD_SANITIZER_ERROR_MESSAGE)

THREAD_SANITIZER_DEFINES = select({
    ("//bazel/config:sanitize_thread_required_settings"): ["THREAD_SANITIZER"],
    ("//bazel/config:tsan_disabled"): [],
}, no_match_error = THREAD_SANITIZER_ERROR_MESSAGE)

UNDEFINED_SANITIZER_DEFINES = select({
    ("//bazel/config:ubsan_enabled"): ["UNDEFINED_BEHAVIOR_SANITIZER"],
    ("//bazel/config:ubsan_disabled"): [],
})

# By default, undefined behavior sanitizer doesn't stop on
# the first error. Make it so. Newer versions of clang
# have renamed the flag.
# However, this flag cannot be included when using the fuzzer sanitizer
# if we want to suppress errors to uncover new ones.

# In dynamic builds, the `vptr` sanitizer check can
# require additional dependency edges. That is very
# inconvenient, because such builds can't use z,defs. The
# result is a very fragile link graph, where refactoring
# the link graph in one place can have surprising effects
# in others. Instead, we just disable the `vptr` sanitizer
# for dynamic builds. We tried some other approaches in
# SERVER-49798 of adding a new descriptor type, but
# that didn't address the fundamental issue that the
# correct link graph for a dynamic+ubsan build isn't the
# same as the correct link graph for a regular dynamic
# build.

UNDEFINED_SANITIZER_COPTS = select({
    "//bazel/config:ubsan_enabled": ["-fsanitize=undefined"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:sanitize_undefined_dynamic_link_settings": ["-fno-sanitize=vptr"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:sanitize_undefined_without_fuzzer_settings": ["-fno-sanitize-recover"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:ubsan_enabled": ["-fsanitize-blacklist=$(location //etc:ubsan_denylist_h)"],
    "//conditions:default": [],
})

UNDEFINED_SANITIZER_LINKFLAGS = select({
    "//bazel/config:ubsan_enabled": ["-fsanitize=undefined"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:sanitize_undefined_dynamic_link_settings": ["-fno-sanitize=vptr"],
    "//conditions:default": [],
})

# Used as both link flags and copts
# Suppress the function sanitizer check for third party libraries, because:
# - mongod (a C++ binary) links in WiredTiger (a C library)
# - If/when mongod--built under ubsan--fails, the sanitizer will by default analyze the failed execution
#   for undefined behavior related to function pointer usage
#   (see https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html#available-checks).
# - When this happens, the sanitizer will attempt to dynamically load to perform the analysis.
# - However, since WT was built as a C library, is not linked with the function sanitizer library symbols
#   despite its C++ dependencies referencing them.
# - This will cause the sanitizer itself to fail, resulting in debug information being unavailable.
# - So by suppressing the function ubsan check, we won't reference symbols defined in the unavailable
#   ubsan function sanitier library and will get useful debugging information.
UBSAN_OPTS_THIRD_PARTY = select({
    "//bazel/config:sanitize_undefined_dynamic_link_settings": ["-fno-sanitize=function"],
    "//conditions:default": [],
})

REQUIRED_SETTINGS_DYNAMIC_LINK_ERROR_MESSAGE = (
    "\nError:\n" +
    "    linking mongo dynamically is not currently supported on Windows"
)

# This is a hack to work around the fact that the cc_library flag additional_compiler_inputs doesn't
# exist in cc_binary. Instead, we add the denylists to srcs as header files to make them visible to
# the compiler executable.
SANITIZER_DENYLIST_HEADERS = select({
    "//bazel/config:asan_enabled": ["//etc:asan_denylist_h"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:msan_enabled": ["//etc:msan_denylist_h"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:tsan_enabled": ["//etc:tsan_denylist_h"],
    "//conditions:default": [],
}) + select({
    "//bazel/config:ubsan_enabled": ["//etc:ubsan_denylist_h"],
    "//conditions:default": [],
})

LINKSTATIC_ENABLED = select({
    "//bazel/config:linkstatic_enabled": True,
    "//bazel/config:linkdynamic_required_settings": False,
}, no_match_error = REQUIRED_SETTINGS_DYNAMIC_LINK_ERROR_MESSAGE)

SEPARATE_DEBUG_ENABLED = select({
    "//bazel/config:separate_debug_enabled": True,
    "//conditions:default": False,
})

TCMALLOC_ERROR_MESSAGE = (
    "\nError:\n" +
    "    Build failed due to unsupported platform for current allocator selection:\n" +
    "    '--//bazel/config:allocator=tcmalloc-google' is supported on linux with aarch64 or x86_64\n" +
    "    '--//bazel/config:allocator=tcmalloc-gperf' is supported on windows or linux, but not macos\n" +
    "    '--//bazel/config:allocator=system' can be used on any platform\n"
)

TCMALLOC_DEPS = select({
    "//bazel/config:tcmalloc_google_enabled": [
        "//src/third_party/tcmalloc:tcmalloc",
        "//src/third_party/tcmalloc:tcmalloc_internal_percpu_tcmalloc",
        "//src/third_party/tcmalloc:tcmalloc_internal_sysinfo",
    ],
    "//bazel/config:tcmalloc_gperf_enabled": ["//src/third_party/gperftools:tcmalloc_minimal"],
    "//bazel/config:system_allocator_enabled": [],
}, no_match_error = TCMALLOC_ERROR_MESSAGE)

TCMALLOC_DEFINES = select({
    "//bazel/config:tcmalloc_google_enabled": ["ABSL_ALLOCATOR_NOTHROW"],
    "//conditions:default": [],
})

#TODO SERVER-84714 add message about using the toolchain version of C++ libs
GLIBCXX_DEBUG_ERROR_MESSAGE = (
    "\nError:\n" +
    "    glibcxx_debug requires these configurations:\n" +
    "    --//bazel/config:dbg=True\n" +
    "    --//bazel/config:use_libcxx=False"
)

GLIBCXX_DEBUG_DEFINES = select({
    ("//bazel/config:use_glibcxx_debug_required_settings"): ["_GLIBCXX_DEBUG"],
    ("//bazel/config:use_glibcxx_debug_disabled"): [],
}, no_match_error = GLIBCXX_DEBUG_ERROR_MESSAGE)

DETECT_ODR_VIOLATIONS_ERROR_MESSAGE = (
    "\nError:\n" +
    "    detect_odr_violations requires these configurations:\n" +
    "    --//bazel/config:opt=off\n" +
    "    --//bazel/config:linker=gold\n"
)

DETECT_ODR_VIOLATIONS_LINKFLAGS = select({
    ("//bazel/config:detect_odr_violations_required_settings"): ["-Wl,--detect-odr-violations"],
    ("//bazel/config:detect_odr_violations_disabled"): [],
}, no_match_error = DETECT_ODR_VIOLATIONS_ERROR_MESSAGE)

# These are added as both copts and linker flags.
GDWARF_FEATURES = select({
    # SCons implementation originally used a compiler check to verify that -gdwarf64 was supported.
    # If this creates incompatibility issues, we may need to fallback to -gdwarf32 in certain cases.
    "//bazel/config:linux_gcc": ["dwarf64"],
    "//bazel/config:linux_clang": ["dwarf32"],
    "//conditions:default": [],
})

DEBUG_TYPES_SECTION_FLAGS = select({
    "//bazel/config:linux_clang_linkstatic": [
        "-fdebug-types-section",
    ],
    "//conditions:default": [],
})

GCC_OR_CLANG_LINKFLAGS = select({
    "//bazel/config:linux_gcc_or_clang": [
        # Explicitly enable GNU build id's if the linker supports it.
        "-Wl,--build-id",

        # Explicitly use the new gnu hash section if the linker offers it.
        "-Wl,--hash-style=gnu",

        # Disallow an executable stack. Also, issue a warning if any files are found that would
        # cause the stack to become executable if the noexecstack flag was not in play, so that we
        # can find them and fix them. We do this here after we check for ld.gold because the
        # --warn-execstack is currently only offered with gold.
        "-Wl,-z,noexecstack",
        "-Wl,--warn-execstack",

        # If possible with the current linker, mark relocations as read-only.
        "-Wl,-z,relro",

        # Disable TBAA optimization
        "-fno-strict-aliasing",
    ],
    "//conditions:default": [],
})

COMPRESS_DEBUG_COPTS = select({
    # Disable debug compression in both the assembler and linker
    # by default.
    "//bazel/config:linux_gcc": [
        "-Wa,--nocompress-debug-sections",
    ],
    "//conditions:default": [],
})

COMPRESS_DEBUG_LINKFLAGS = select({
    # Disable debug compression in both the assembler and linker
    # by default.
    "@platforms//os:linux": [
        "-Wl,--compress-debug-sections=none",
    ],
    "//conditions:default": [],
})

# Avoid deduping symbols on OS X debug builds, as it takes a long time.
DEDUPE_SYMBOL_LINKFLAGS = select({
    "//bazel/config:macos_opt_off": ["-Wl,-no_deduplicate"],
    "//conditions:default": [],
})

DISABLE_SOURCE_WARNING_AS_ERRORS_COPTS = select({
    "//bazel/config:disable_warnings_as_errors_linux": ["-Werror"],
    # TODO(SERVER-90183): Enable once MacOS has a custom Bazel toolchain config.
    # "//bazel/config:disable_warnings_as_errors_macos": ["-Werror"],
    "//bazel/config:disable_warnings_as_errors_windows": ["/WX"],
    "//bazel/config:warnings_as_errors_disabled": [],
    "//conditions:default": [],
})

# Enable sized deallocation support.
# Bazel doesn't allow for defining C++-only flags without a custom toolchain config. This is setup
# in the Linux toolchain, but currently there is no custom MacOS toolchain. Enabling warnings-as-errors will fail
# the build if this flag is passed to the compiler when building C code.
# Define it here on MacOS only to allow us to configure warnings-as-errors on Linux.
# TODO(SERVER-90183): Remove this once custom toolchain configuration is implemented on MacOS.
FSIZED_DEALLOCATION_COPT = select({
    "@platforms//os:macos": ["-fsized-deallocation"],
    "//conditions:default": [],
})

DISABLE_SOURCE_WARNING_AS_ERRORS_LINKFLAGS = select({
    "//bazel/config:disable_warnings_as_errors_linux": ["-Wl,--fatal-warnings"],
    "//bazel/config:warnings_as_errors_disabled": [],
    "//conditions:default": [],
})

MTUNE_MARCH_COPTS = select({
    # If we are enabling vectorization in sandybridge mode, we'd
    # rather not hit the 256 wide vector instructions because the
    # heavy versions can cause clock speed reductions.
    "//bazel/config:linux_x86_64": [
        "-march=sandybridge",
        "-mtune=generic",
        "-mprefer-vector-width=128",
    ],
    "//bazel/config:linux_aarch64": [
        "-march=armv8.2-a",
        "-mtune=generic",
    ],
    "//bazel/config:linux_ppc64le": [
        "-mcpu=power8",
        "-mtune=power8",
        "-mcmodel=medium",
    ],
    "//bazel/config:linux_s390x": [
        "-march=z196",
        "-mtune=zEC12",
    ],
    "//conditions:default": [],
})

THIN_LTO_FLAGS = select({
    "//bazel/config:thin_lto_enabled": ["-flto=thin"],
    "//conditions:default": [],
})

MONGO_GLOBAL_INCLUDE_DIRECTORIES = [
    "-Isrc",
    "-I$(GENDIR)/src",
    "-Isrc/third_party/immer/dist",
    "-Isrc/third_party/SafeInt",
]

MONGO_GLOBAL_ACCESSIBLE_HEADERS = [
    "//src/third_party/immer:headers",
    "//src/third_party/SafeInt:headers",
]

MONGO_GLOBAL_SRC_DEPS = [
    "//src/third_party/abseil-cpp:absl_base",
    "//src/third_party/boost:boost_system",
    "//src/third_party/croaring:croaring",
    "//src/third_party/fmt:fmt",
    "//src/third_party/libstemmer_c:stemmer",
    "//src/third_party/murmurhash3:murmurhash3",
    "//src/third_party/tomcrypt-1.18.2:tomcrypt",
]

MONGO_GLOBAL_DEFINES = DEBUG_DEFINES + LIBCXX_DEFINES + ADDRESS_SANITIZER_DEFINES + \
                       THREAD_SANITIZER_DEFINES + UNDEFINED_SANITIZER_DEFINES + GLIBCXX_DEBUG_DEFINES + \
                       WINDOWS_DEFINES + MACOS_DEFINES + TCMALLOC_DEFINES + LINUX_DEFINES + GCC_OPT_DEFINES + \
                       BOOST_DEFINES + ABSEIL_DEFINES + PCRE2_DEFINES + SAFEINT_DEFINES

MONGO_GLOBAL_COPTS = MONGO_GLOBAL_INCLUDE_DIRECTORIES + WINDOWS_COPTS + LIBCXX_COPTS + ADDRESS_SANITIZER_COPTS + \
                     MEMORY_SANITIZER_COPTS + FUZZER_SANITIZER_COPTS + UNDEFINED_SANITIZER_COPTS + \
                     THREAD_SANITIZER_COPTS + ANY_SANITIZER_AVAILABLE_COPTS + LINUX_OPT_COPTS + \
                     GCC_OR_CLANG_WARNINGS_COPTS + GCC_OR_CLANG_GENERAL_COPTS + \
                     FLOATING_POINT_COPTS + MACOS_WARNINGS_COPTS + CLANG_WARNINGS_COPTS + \
                     CLANG_FNO_LIMIT_DEBUG_INFO + COMPRESS_DEBUG_COPTS + DEBUG_TYPES_SECTION_FLAGS + \
                     IMPLICIT_FALLTHROUGH_COPTS + MTUNE_MARCH_COPTS + DISABLE_SOURCE_WARNING_AS_ERRORS_COPTS + \
                     FSIZED_DEALLOCATION_COPT + THIN_LTO_FLAGS

MONGO_GLOBAL_LINKFLAGS = MEMORY_SANITIZER_LINKFLAGS + ADDRESS_SANITIZER_LINKFLAGS + FUZZER_SANITIZER_LINKFLAGS + \
                         UNDEFINED_SANITIZER_LINKFLAGS + THREAD_SANITIZER_LINKFLAGS + \
                         LIBCXX_LINKFLAGS + LINKER_LINKFLAGS + DETECT_ODR_VIOLATIONS_LINKFLAGS + WINDOWS_LINKFLAGS + \
                         BIND_AT_LOAD_LINKFLAGS + RDYNAMIC_LINKFLAG + LINUX_PTHREAD_LINKFLAG + \
                         EXTRA_GLOBAL_LIBS_LINKFLAGS + ANY_SANITIZER_AVAILABLE_LINKFLAGS + ANY_SANITIZER_GCC_LINKFLAGS + \
                         GCC_OR_CLANG_LINKFLAGS + COMPRESS_DEBUG_LINKFLAGS + DEDUPE_SYMBOL_LINKFLAGS + \
                         DEBUG_TYPES_SECTION_FLAGS + DISABLE_SOURCE_WARNING_AS_ERRORS_LINKFLAGS + THIN_LTO_FLAGS

MONGO_GLOBAL_FEATURES = GDWARF_FEATURES + DWARF_VERSION_FEATURES

MONGO_COPTS_THIRD_PARTY = UBSAN_OPTS_THIRD_PARTY

MONGO_LINKFLAGS_THIRD_PARTY = UBSAN_OPTS_THIRD_PARTY

def force_includes_copt(package_name, name):
    if package_name.startswith("src/mongo"):
        basic_h = "mongo/platform/basic.h"
        return select({
            "@platforms//os:windows": ["/FI", basic_h],
            "//conditions:default": ["-include", basic_h],
        })

    if package_name.startswith("src/third_party/mozjs"):
        return select({
            "//bazel/config:linux_aarch64": ["-include", "third_party/mozjs/platform/aarch64/linux/build/js-confdefs.h"],
            "//bazel/config:linux_x86_64": ["-include", "third_party/mozjs/platform/x86_64/linux/build/js-confdefs.h"],
            "//bazel/config:linux_ppc64le": ["-include", "third_party/mozjs/platform/ppc64le/linux/build/js-confdefs.h"],
            "//bazel/config:linux_s390x": ["-include", "third_party/mozjs/platform/s390x/linux/build/js-confdefs.h"],
            "//bazel/config:windows_x86_64": ["/FI", "third_party/mozjs/platform/x86_64/windows/build/js-confdefs.h"],
            "//bazel/config:macos_x86_64": ["-include", "third_party/mozjs/platform/x86_64/macOS/build/js-confdefs.h"],
            "//bazel/config:macos_aarch64": ["-include", "third_party/mozjs/platform/aarch64/macOS/build/js-confdefs.h"],
        })

    if name in ["scripting", "scripting_mozjs_test", "encrypted_dbclient"]:
        return select({
            "//bazel/config:linux_aarch64": ["-include", "third_party/mozjs/platform/aarch64/linux/build/js-config.h"],
            "//bazel/config:linux_x86_64": ["-include", "third_party/mozjs/platform/x86_64/linux/build/js-config.h"],
            "//bazel/config:linux_ppc64le": ["-include", "third_party/mozjs/platform/ppc64le/linux/build/js-config.h"],
            "//bazel/config:linux_s390x": ["-include", "third_party/mozjs/platform/s390x/linux/build/js-config.h"],
            "//bazel/config:windows_x86_64": ["/FI", "third_party/mozjs/platform/x86_64/windows/build/js-config.h"],
            "//bazel/config:macos_x86_64": ["-include", "third_party/mozjs/platform/x86_64/macOS/build/js-config.h"],
            "//bazel/config:macos_aarch64": ["-include", "third_party/mozjs/platform/aarch64/macOS/build/js-config.h"],
        })

    return []

def force_includes_hdr(package_name, name):
    if package_name.startswith("src/mongo"):
        return select({
            "@platforms//os:windows": ["//src/mongo/platform:basic.h", "//src/mongo/platform:windows_basic.h"],
            "//conditions:default": ["//src/mongo/platform:basic.h"],
        })
        return

    if package_name.startswith("src/third_party/mozjs"):
        return select({
            "//bazel/config:linux_aarch64": ["//src/third_party/mozjs:platform/aarch64/linux/build/js-confdefs.h"],
            "//bazel/config:linux_x86_64": ["//src/third_party/mozjs:platform/x86_64/linux/build/js-confdefs.h"],
            "//bazel/config:linux_ppc64le": ["//src/third_party/mozjs:platform/ppc64le/linux/build/js-confdefs.h"],
            "//bazel/config:linux_s390x": ["//src/third_party/mozjs:platform/s390x/linux/build/js-confdefs.h"],
            "//bazel/config:windows_x86_64": ["//src/third_party/mozjs:/platform/x86_64/windows/build/js-confdefs.h"],
            "//bazel/config:macos_x86_64": ["//src/third_party/mozjs:platform/x86_64/macOS/build/js-confdefs.h"],
            "//bazel/config:macos_aarch64": ["//src/third_party/mozjs:platform/aarch64/macOS/build/js-confdefs.h"],
        })

    if name in ["scripting", "scripting_mozjs_test", "encrypted_dbclient"]:
        return select({
            "//bazel/config:linux_aarch64": ["//src/third_party/mozjs:platform/aarch64/linux/build/js-config.h"],
            "//bazel/config:linux_x86_64": ["//src/third_party/mozjs:platform/x86_64/linux/build/js-config.h"],
            "//bazel/config:linux_ppc64le": ["//src/third_party/mozjs:platform/ppc64le/linux/build/js-config.h"],
            "//bazel/config:linux_s390x": ["//src/third_party/mozjs:platform/s390x/linux/build/js-config.h"],
            "//bazel/config:windows_x86_64": ["//src/third_party/mozjs:/platform/x86_64/windows/build/js-config.h"],
            "//bazel/config:macos_x86_64": ["//src/third_party/mozjs:platform/x86_64/macOS/build/js-config.h"],
            "//bazel/config:macos_aarch64": ["//src/third_party/mozjs:platform/aarch64/macOS/build/js-config.h"],
        })

    return []

def package_specific_copt(package_name):
    if package_name.startswith("src/third_party"):
        return MONGO_COPTS_THIRD_PARTY

    return []

def package_specific_linkflag(package_name):
    if package_name.startswith("src/third_party"):
        return MONGO_LINKFLAGS_THIRD_PARTY

    return []

def mongo_cc_library(
        name,
        srcs = [],
        hdrs = [],
        deps = [],
        testonly = False,
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        mongo_api_name = None,
        target_compatible_with = [],
        skip_global_deps = [],
        non_transitive_dyn_linkopts = [],
        defines = [],
        additional_linker_inputs = [],
        features = []):
    """Wrapper around cc_library.

    Args:
      name: The name of the library the target is compiling.
      srcs: The source files to build.
      hdrs: The headers files of the target library.
      deps: The targets the library depends on.
      testonly: Whether or not the target is purely for tests.
      visibility: The visibility of the target library.
      data: Data targets the library depends on.
      tags: Tags to add to the rule.
      copts: Any extra compiler options to pass in.
      linkopts: Any extra link options to pass in. These are applied transitively to all targets that depend on this target.
      includes: Any directory which should be exported to dependents, will be prefixed with the package path
      linkstatic: Whether or not linkstatic should be passed to the native bazel cc_test rule. This argument
        is currently not supported. The mongo build must link entirely statically or entirely dynamically. This can be
        configured via //config/bazel:linkstatic.
      local_defines: macro definitions added to the compile line when building any source in this target, but not to the compile
        line of targets that depend on this.
      skip_global_deps: Globally injected dependencies to skip adding as a dependency (options: "libunwind", "allocator").
      non_transitive_dyn_linkopts: Any extra link options to pass in when linking dynamically. Unlike linkopts these are not
        applied transitively to all targets depending on this target, and are only used when linking this target itself.
        See https://jira.mongodb.org/browse/SERVER-89047 for motivation.
      defines: macro definitions added to the compile line when building any source in this target, as well as the compile
        line of targets that depend on this.
      additional_linker_inputs: Any additional files that you may want to pass to the linker, for example, linker scripts.
    """

    if linkstatic == True:
        fail("""Linking specific targets statically is not supported.
        The mongo build must link entirely statically or entirely dynamically.
        This can be configured via //config/bazel:linkstatic.""")

    if "libunwind" not in skip_global_deps:
        deps += LIBUNWIND_DEPS

    if "allocator" not in skip_global_deps:
        deps += TCMALLOC_DEPS

    if native.package_name().startswith("src/mongo"):
        hdrs = hdrs + ["//src/mongo:mongo_config_header"]
        if name != "boost_assert_shim":
            deps += MONGO_GLOBAL_SRC_DEPS

    fincludes_copt = force_includes_copt(native.package_name(), name)
    fincludes_hdr = force_includes_hdr(native.package_name(), name)
    package_specific_copts = package_specific_copt(native.package_name())
    package_specific_linkflags = package_specific_linkflag(native.package_name())

    if mongo_api_name:
        visibility_support_defines_list = ["MONGO_USE_VISIBILITY", "MONGO_API_" + mongo_api_name]
        visibility_support_shared_lib_flags_list = ["-fvisibility=hidden"]
    else:
        visibility_support_defines_list = ["MONGO_USE_VISIBILITY"]
        visibility_support_shared_lib_flags_list = []

    visibility_support_defines = select({
        ("//bazel/config:visibility_support_enabled_dynamic_linking_setting"): visibility_support_defines_list,
        "//conditions:default": [],
    })

    visibility_support_shared_flags = select({
        ("//bazel/config:visibility_support_enabled_dynamic_linking_non_windows_setting"): visibility_support_shared_lib_flags_list,
        "//conditions:default": [],
    })

    linux_rpath_flags = ["-Wl,-z,origin", "-Wl,--enable-new-dtags", "-Wl,-rpath,\\$ORIGIN/../lib", "-Wl,-h,lib" + name + ".so"]
    macos_rpath_flags = ["-Wl,-rpath,\\$ORIGIN/../lib", "-Wl,-install_name,@rpath/lib" + name + ".dylib"]

    rpath_flags = select({
        "//bazel/config:linux_aarch64": linux_rpath_flags,
        "//bazel/config:linux_x86_64": linux_rpath_flags,
        "//bazel/config:linux_ppc64le": linux_rpath_flags,
        "//bazel/config:linux_s390x": linux_rpath_flags,
        "//bazel/config:windows_x86_64": [],
        "//bazel/config:macos_x86_64": macos_rpath_flags,
        "//bazel/config:macos_aarch64": macos_rpath_flags,
    })

    # Create a cc_library entry to generate a shared archive of the target.
    native.cc_library(
        name = name + SHARED_ARCHIVE_SUFFIX,
        srcs = srcs + SANITIZER_DENYLIST_HEADERS,
        hdrs = hdrs + fincludes_hdr + MONGO_GLOBAL_ACCESSIBLE_HEADERS,
        deps = deps,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + package_specific_copts + copts + fincludes_copt,
        data = data,
        tags = tags,
        linkopts = MONGO_GLOBAL_LINKFLAGS + package_specific_linkflags + linkopts,
        linkstatic = True,
        local_defines = MONGO_GLOBAL_DEFINES + visibility_support_defines + local_defines,
        defines = defines,
        includes = includes,
        features = MONGO_GLOBAL_FEATURES + ["supports_pic", "pic"] + features,
        target_compatible_with = select({
            "//bazel/config:shared_archive_enabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }) + target_compatible_with,
        additional_linker_inputs = additional_linker_inputs,
    )

    native.cc_library(
        name = name + WITH_DEBUG_SUFFIX,
        srcs = srcs + SANITIZER_DENYLIST_HEADERS,
        hdrs = hdrs + fincludes_hdr + MONGO_GLOBAL_ACCESSIBLE_HEADERS,
        deps = deps,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + package_specific_copts + copts + fincludes_copt,
        data = data,
        tags = tags,
        linkopts = MONGO_GLOBAL_LINKFLAGS + package_specific_linkflags + linkopts,
        linkstatic = True,
        local_defines = MONGO_GLOBAL_DEFINES + local_defines,
        defines = defines,
        includes = includes,
        features = MONGO_GLOBAL_FEATURES + select({
            "//bazel/config:linkstatic_disabled": ["supports_pic", "pic"],
            "//bazel/config:shared_archive_enabled": ["supports_pic", "pic"],
            "//conditions:default": ["pie"],
        }) + features,
        target_compatible_with = target_compatible_with,
        additional_linker_inputs = additional_linker_inputs,
    )

    # Creates a shared library version of our target only if //bazel/config:linkstatic_disabled is true.
    # This uses the CcSharedLibraryInfo provided from extract_debuginfo to allow it to declare all dependencies in
    # dynamic_deps.
    native.cc_shared_library(
        name = name + CC_SHARED_LIBRARY_SUFFIX + WITH_DEBUG_SUFFIX,
        deps = [name + WITH_DEBUG_SUFFIX],
        visibility = visibility,
        tags = tags,
        user_link_flags = MONGO_GLOBAL_LINKFLAGS + package_specific_linkflags + non_transitive_dyn_linkopts + rpath_flags + visibility_support_shared_flags,
        target_compatible_with = select({
            "//bazel/config:linkstatic_disabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }) + target_compatible_with,
        dynamic_deps = deps,
        features = select({
            "@platforms//os:windows": ["generate_pdb_file"],
            "//conditions:default": [],
        }),
        additional_linker_inputs = additional_linker_inputs,
    )

    extract_debuginfo(
        name = name,
        binary_with_debug = ":" + name + WITH_DEBUG_SUFFIX,
        type = "library",
        enabled = SEPARATE_DEBUG_ENABLED,
        cc_shared_library = select({
            "//bazel/config:linkstatic_disabled": ":" + name + CC_SHARED_LIBRARY_SUFFIX + WITH_DEBUG_SUFFIX,
            "//conditions:default": None,
        }),
        shared_archive = select({
            "//bazel/config:shared_archive_enabled": ":" + name + SHARED_ARCHIVE_SUFFIX,
            "//conditions:default": None,
        }),
        deps = deps,
    )

def mongo_cc_binary(
        name,
        srcs = [],
        deps = [],
        testonly = False,
        visibility = None,
        data = [],
        tags = [],
        copts = [],
        linkopts = [],
        includes = [],
        linkstatic = False,
        local_defines = [],
        target_compatible_with = [],
        defines = [],
        additional_linker_inputs = [],
        features = []):
    """Wrapper around cc_binary.

    Args:
      name: The name of the library the target is compiling.
      srcs: The source files to build.
      deps: The targets the library depends on.
      testonly: Whether or not the target is purely for tests.
      visibility: The visibility of the target library.
      data: Data targets the library depends on.
      tags: Tags to add to the rule.
      copts: Any extra compiler options to pass in.
      linkopts: Any extra link options to pass in.
      includes: Any directory which should be exported to dependents, will be prefixed with the package path
      linkstatic: Whether or not linkstatic should be passed to the native bazel cc_test rule. This argument
        is currently not supported. The mongo build must link entirely statically or entirely dynamically. This can be
        configured via //config/bazel:linkstatic.
      local_defines: macro definitions passed to all source and header files.
      defines: macro definitions added to the compile line when building any source in this target, as well as the compile
        line of targets that depend on this.
      additional_linker_inputs: Any additional files that you may want to pass to the linker, for example, linker scripts.
    """

    if linkstatic == True:
        fail("""Linking specific targets statically is not supported.
        The mongo build must link entirely statically or entirely dynamically.
        This can be configured via //config/bazel:linkstatic.""")

    if native.package_name().startswith("src/mongo"):
        srcs = srcs + ["//src/mongo:mongo_config_header"]
        deps += MONGO_GLOBAL_SRC_DEPS

    fincludes_copt = force_includes_copt(native.package_name(), name)
    fincludes_hdr = force_includes_hdr(native.package_name(), name)
    package_specific_copts = package_specific_copt(native.package_name())
    package_specific_linkflags = package_specific_linkflag(native.package_name())

    all_deps = deps + LIBUNWIND_DEPS + TCMALLOC_DEPS

    linux_rpath_flags = ["-Wl,-z,origin", "-Wl,--enable-new-dtags", "-Wl,-rpath,\\$$ORIGIN/../lib"]
    macos_rpath_flags = ["-Wl,-rpath,\\$$ORIGIN/../lib"]

    rpath_flags = select({
        "//bazel/config:linux_aarch64": linux_rpath_flags,
        "//bazel/config:linux_x86_64": linux_rpath_flags,
        "//bazel/config:linux_ppc64le": linux_rpath_flags,
        "//bazel/config:linux_s390x": linux_rpath_flags,
        "//bazel/config:windows_x86_64": [],
        "//bazel/config:macos_x86_64": macos_rpath_flags,
        "//bazel/config:macos_aarch64": macos_rpath_flags,
    })

    native.cc_binary(
        name = name + WITH_DEBUG_SUFFIX,
        srcs = srcs + fincludes_hdr + MONGO_GLOBAL_ACCESSIBLE_HEADERS + SANITIZER_DENYLIST_HEADERS,
        deps = all_deps,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + package_specific_copts + copts + fincludes_copt,
        data = data,
        tags = tags,
        linkopts = MONGO_GLOBAL_LINKFLAGS + package_specific_linkflags + linkopts + rpath_flags,
        linkstatic = LINKSTATIC_ENABLED,
        local_defines = MONGO_GLOBAL_DEFINES + local_defines,
        defines = defines,
        includes = includes,
        features = MONGO_GLOBAL_FEATURES + ["pie"] + features + select({
            "@platforms//os:windows": ["generate_pdb_file"],
            "//conditions:default": [],
        }),
        dynamic_deps = select({
            "//bazel/config:linkstatic_disabled": deps,
            "//conditions:default": [],
        }),
        target_compatible_with = target_compatible_with,
        additional_linker_inputs = additional_linker_inputs,
    )

    extract_debuginfo_binary(
        name = name,
        binary_with_debug = ":" + name + WITH_DEBUG_SUFFIX,
        type = "program",
        enabled = SEPARATE_DEBUG_ENABLED,
        deps = all_deps,
    )

IdlInfo = provider(
    fields = {
        "idl_deps": "depset of idl files",
    },
)

def idl_generator_impl(ctx):
    base = ctx.attr.src.files.to_list()[0].basename.removesuffix(".idl")
    gen_source = ctx.actions.declare_file(base + "_gen.cpp")
    gen_header = ctx.actions.declare_file(base + "_gen.h")

    python = ctx.toolchains["@bazel_tools//tools/python:toolchain_type"].py3_runtime
    idlc_path = ctx.attr.idlc.files.to_list()[0].path
    dep_depsets = [dep[IdlInfo].idl_deps for dep in ctx.attr.deps]

    # collect deps from python modules and setup the corresponding
    # path so all modules can be found by the toolchain.
    python_path = []
    for py_dep in ctx.attr.py_deps:
        for dep in py_dep[PyInfo].transitive_sources.to_list():
            if dep.path not in python_path:
                python_path.append(dep.path)
    py_depsets = [py_dep[PyInfo].transitive_sources for py_dep in ctx.attr.py_deps]

    inputs = depset(transitive = [
        ctx.attr.src.files,
        ctx.attr.idlc.files,
        python.files,
    ] + dep_depsets + py_depsets)

    ctx.actions.run(
        executable = python.interpreter.path,
        outputs = [gen_source, gen_header],
        inputs = inputs,
        arguments = [
            "buildscripts/idl/idlc.py",
            "--include",
            "src",
            "--base_dir",
            ctx.bin_dir.path + "/src",
            "--target_arch",
            ctx.var["TARGET_CPU"],
            "--header",
            gen_header.path,
            "--output",
            gen_source.path,
            ctx.attr.src.files.to_list()[0].path,
        ],
        mnemonic = "IdlcGenerator",
        env = {"PYTHONPATH": ctx.configuration.host_path_separator.join(python_path)},
    )

    return [
        DefaultInfo(
            files = depset([gen_source, gen_header]),
        ),
        IdlInfo(
            idl_deps = depset(ctx.attr.src.files.to_list(), transitive = [dep[IdlInfo].idl_deps for dep in ctx.attr.deps]),
        ),
    ]

idl_generator = rule(
    idl_generator_impl,
    attrs = {
        "src": attr.label(
            doc = "The idl file to generate cpp/h files from.",
            allow_single_file = True,
        ),
        "idlc": attr.label(
            doc = "The idlc generator to use.",
            default = "//buildscripts/idl:idlc",
        ),
        "py_deps": attr.label_list(
            doc = "Python modules that should be imported.",
            providers = [PyInfo],
            default = [dependency("pyyaml", group = "core"), dependency("pymongo", group = "core")],
        ),
        "deps": attr.label_list(
            doc = "Other idl files that need to be imported.",
            providers = [IdlInfo],
        ),
    },
    doc = "Generates header/source files from IDL files.",
    toolchains = ["@bazel_tools//tools/python:toolchain_type"],
    fragments = ["py"],
)

def symlink_impl(ctx):
    ctx.actions.symlink(
        output = ctx.outputs.output,
        target_file = ctx.attr.input.files.to_list()[0],
    )

    return [DefaultInfo(files = depset([ctx.outputs.output]))]

symlink = rule(
    symlink_impl,
    attrs = {
        "input": attr.label(
            doc = "The File that the output symlink will point to.",
            allow_single_file = True,
        ),
        "output": attr.output(
            doc = "The output of this rule.",
        ),
    },
)
