# Common mongo-specific bazel build rules intended to be used in individual BUILD files in the "src/" subtree.
load("@poetry//:dependencies.bzl", "dependency")

# config selection
load("@bazel_skylib//lib:selects.bzl", "selects")
load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain")
load("//bazel:separate_debug.bzl", "CC_SHARED_LIBRARY_SUFFIX", "WITH_DEBUG_SUFFIX", "extract_debuginfo", "extract_debuginfo_binary")
# === Windows-specific compilation settings ===

# /RTC1              Enable Stack Frame Run-Time Error Checking; Reports when a variable is used without having been initialized (implies /Od: no optimizations)
# /O1                optimize for size
# /O2                optimize for speed (as opposed to size)
# /Oy-               disable frame pointer optimization (overrides /O2, only affects 32-bit)
# /INCREMENTAL: NO - disable incremental link - avoid the level of indirection for function calls

# /pdbpagesize:16384
#     windows non optimized builds will cause the PDB to blow up in size,
#     this allows a larger PDB. The flag is undocumented at the time of writing
#     but the microsoft thread which brought about its creation can be found here:
#     https://developercommunity.visualstudio.com/t/pdb-limit-of-4-gib-is-likely-to-be-a-problem-in-a/904784
#
#     Without this flag MSVC will report a red herring error message, about disk space or invalid path.

WINDOWS_DBG_COPTS = [
    "/MDd",
    "/RTC1",
    "/Od",
]

WINDOWS_OPT_ON_COPTS = [
    "/MD",
    "/O2",
    "/Oy-",
]

WINDOWS_OPT_OFF_COPTS = [
    "/MD",
    "/Od",
]

WINDOWS_OPT_DBG_COPTS = [
    "/MD",
    "/RTC1",
    "/Ox",
    "/Zo",
    "/Oy-",
]

WINDOWS_OPT_SIZE_COPTS = [
    "/MD",
    "/Os",
    "/Oy-",
]

WINDOWS_RELEASE_COPTS = [
    "/MD",
    "/Od",
]

WINDOWS_DBG_LINKFLAGS = [
    "/pdbpagesize:16384",
]

WINDOWS_OPT_OFF_LINKFLAGS = [
    "/pdbpagesize:16384",
]

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

WINDOWS_COPTS = select({
    "//bazel/config:windows_dbg": WINDOWS_DBG_COPTS,
    "//bazel/config:windows_opt_on": WINDOWS_OPT_ON_COPTS,
    "//bazel/config:windows_opt_off": WINDOWS_OPT_OFF_COPTS,
    "//bazel/config:windows_opt_debug": WINDOWS_OPT_DBG_COPTS,
    "//bazel/config:windows_opt_size": WINDOWS_OPT_SIZE_COPTS,
    "//bazel/config:windows_release": WINDOWS_RELEASE_COPTS,
    "//conditions:default": [],
})

WINDOWS_LINKFLAGS = select({
    "//bazel/config:windows_dbg": WINDOWS_DBG_LINKFLAGS,
    "//bazel/config:windows_opt_off": WINDOWS_OPT_OFF_LINKFLAGS,
    "//conditions:default": [],
})

DEBUG_DEFINES = select({
    "//bazel/config:dbg": ["MONGO_CONFIG_DEBUG_BUILD"],
    "//conditions:default": ["NDEBUG"],
})

LINKER_ERROR_MESSAGE = (
    "\nError:\n" +
    "    --//bazel/config:linker=lld is not supported on s390x"
)

LINKER_LINKFLAGS = select({
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
    "//bazel/config:_libunwind_auto": [],
}, no_match_error = REQUIRED_SETTINGS_LIBUNWIND_ERROR_MESSAGE)

LIBUNWIND_DEFINES = select({
    "//bazel/config:libunwind_enabled": ["MONGO_CONFIG_USE_LIBUNWIND"],
    "//bazel/config:_libunwind_off": [],
    "//bazel/config:_libunwind_auto": [],
}, no_match_error = REQUIRED_SETTINGS_LIBUNWIND_ERROR_MESSAGE)

REQUIRED_SETTINGS_SANITIZER_ERROR_MESSAGE = (
    "\nError:\n" +
    "    any sanitizer requires these configurations:\n" +
    "    --//bazel/config:compiler_type=clang\n" +
    "    --//bazel/config:build_mode=opt_on [OR] --//bazel/config:build_mode=opt_debug [OR] --//bazel/config:build_mode=dbg"
)

# -fno-omit-frame-pointer should be added if any sanitizer flag is used by user
ANY_SANITIZER_AVAILABLE_COPTS = select(
    {
        "//bazel/config:no_enabled_sanitizer": [],
        "//bazel/config:any_sanitizer_required_setting": ["-fno-omit-frame-pointer"],
    },
    no_match_error = REQUIRED_SETTINGS_SANITIZER_ERROR_MESSAGE,
)

SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE = (
    "\nError:\n" +
    "    address and memory sanitizers require these configurations:\n" +
    "    --//bazel/config:allocator=system\n"
)

ADDRESS_SANITIZER_COPTS = select(
    {
        ("//bazel/config:sanitize_address_required_settings"): ["-fsanitize=address"],
        "//bazel/config:asan_disabled": [],
    },
    no_match_error = SYSTEM_ALLOCATOR_SANITIZER_ERROR_MESSAGE,
)

ADDRESS_SANITIZER_LINKFLAGS = select(
    {
        ("//bazel/config:sanitize_address_required_settings"): ["-fsanitize=address"],
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
        ("//bazel/config:sanitize_memory_required_settings"): ["-fsanitize=memory", "-fsanitize-memory-track-origins"],
        ("//bazel/config:msan_disabled"): [],
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
    ("//bazel/config:sanitize_thread_required_settings"): ["-fsanitize=thread"],
    ("//bazel/config:tsan_disabled"): [],
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
UNDEFINED_SANITIZER_COPTS = select({
    ("//bazel/config:sanitize_undefined_without_fuzzer_settings"): ["-fno-sanitize-recover"],
    ("//conditions:default"): [],
}) + select({
    ("//bazel/config:sanitize_undefined_dynamic_link_settings"): ["-fno-sanitize=vptr"],
    ("//conditions:default"): [],
}) + select({
    ("//bazel/config:ubsan_enabled"): ["-fsanitize=undefined"],
    ("//bazel/config:ubsan_disabled"): [],
}, no_match_error = GENERIC_SANITIZER_ERROR_MESSAGE + "undefined")

UNDEFINED_SANITIZER_LINKFLAGS = select({
    ("//bazel/config:sanitize_undefined_dynamic_link_settings"): ["-fno-sanitize=vptr"],
    ("//conditions:default"): [],
}) + select({
    ("//bazel/config:ubsan_enabled"): ["-fsanitize=undefined"],
    ("//bazel/config:ubsan_disabled"): [],
}, no_match_error = GENERIC_SANITIZER_ERROR_MESSAGE + "undefined")

REQUIRED_SETTINGS_DYNAMIC_LINK_ERROR_MESSAGE = (
    "\nError:\n" +
    "    linking mongo dynamically is not currently supported on Windows"
)

LINKSTATIC_ENABLED = select({
    "//bazel/config:linkstatic_enabled": True,
    "//bazel/config:linkdynamic_required_settings": False,
}, no_match_error = REQUIRED_SETTINGS_DYNAMIC_LINK_ERROR_MESSAGE)

SEPARATE_DEBUG_ENABLED = select({
    "//bazel/config:separate_debug_enabled": True,
    "//conditions:default": False,
})

TCMALLOC_DEPS = select({
    "//bazel/config:tcmalloc_allocator": ["//src/third_party/gperftools:tcmalloc_minimal"],
    "//bazel/config:auto_allocator_windows": ["//src/third_party/gperftools:tcmalloc_minimal"],
    "//bazel/config:auto_allocator_linux": ["//src/third_party/gperftools:tcmalloc_minimal"],
    "//conditions:default": [],
})

#TODO SERVER-84714 add message about using the toolchain version of C++ libs
GLIBCXX_DEBUG_ERROR_MESSAGE = (
    "\nError:\n" +
    "    glibcxx_debug requires these configurations:\n" +
    "    --//bazel/config:build_mode=dbg\n" +
    "    --//bazel/config:use_libcxx=False"
)

GLIBCXX_DEBUG_DEFINES = select({
    ("//bazel/config:use_glibcxx_debug_required_settings"): ["_GLIBCXX_DEBUG"],
    ("//bazel/config:use_glibcxx_debug_disabled"): [],
}, no_match_error = GLIBCXX_DEBUG_ERROR_MESSAGE)

DETECT_ODR_VIOLATIONS_ERROR_MESSAGE = (
    "\nError:\n" +
    "    detect_odr_violations requires these configurations:\n" +
    "    --//bazel/config:build_mode=opt_off\n" +
    "    --//bazel/config:linker=gold\n"
)

DETECT_ODR_VIOLATIONS_LINKFLAGS = select({
    ("//bazel/config:detect_odr_violations_required_settings"): ["-Wl,--detect-odr-violations"],
    ("//bazel/config:detect_odr_violations_disabled"): [],
}, no_match_error = DETECT_ODR_VIOLATIONS_ERROR_MESSAGE)

MONGO_GLOBAL_DEFINES = DEBUG_DEFINES + LIBCXX_DEFINES + ADDRESS_SANITIZER_DEFINES + \
                       THREAD_SANITIZER_DEFINES + UNDEFINED_SANITIZER_DEFINES + GLIBCXX_DEBUG_DEFINES

MONGO_GLOBAL_COPTS = ["-Isrc"] + WINDOWS_COPTS + LIBCXX_COPTS + ADDRESS_SANITIZER_COPTS + \
                     MEMORY_SANITIZER_COPTS + FUZZER_SANITIZER_COPTS + UNDEFINED_SANITIZER_COPTS + \
                     THREAD_SANITIZER_COPTS + ANY_SANITIZER_AVAILABLE_COPTS

MONGO_GLOBAL_LINKFLAGS = MEMORY_SANITIZER_LINKFLAGS + ADDRESS_SANITIZER_LINKFLAGS + FUZZER_SANITIZER_LINKFLAGS + \
                         UNDEFINED_SANITIZER_LINKFLAGS + THREAD_SANITIZER_LINKFLAGS + \
                         LIBCXX_LINKFLAGS + LINKER_LINKFLAGS + DETECT_ODR_VIOLATIONS_LINKFLAGS + WINDOWS_LINKFLAGS

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
        mongo_api_name = None):
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
      linkopts: Any extra link options to pass in.
      includes: Any directory which should be exported to dependents, will be prefixed with the package path
      linkstatic: Whether or not linkstatic should be passed to the native bazel cc_test rule. This argument
        is currently not supported. The mongo build must link entirely statically or entirely dynamically. This can be
        configured via //config/bazel:linkstatic.
      local_defines: macro definitions passed to all source and header files.
    """

    if linkstatic == True:
        fail("""Linking specific targets statically is not supported.
        The mongo build must link entirely statically or entirely dynamically.
        This can be configured via //config/bazel:linkstatic.""")

    # Avoid injecting into unwind/libunwind_asm to avoid a circular dependency.
    if name not in ["unwind", "tcmalloc_minimal"]:
        deps += LIBUNWIND_DEPS
        local_defines += LIBUNWIND_DEFINES

    fincludes_copt = force_includes_copt(native.package_name(), name)
    fincludes_hdr = force_includes_hdr(native.package_name(), name)

    if name != "tcmalloc_minimal":
        deps += TCMALLOC_DEPS

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
    macos_rpath_flags = ["-Wl,-rpath,\\$ORIGIN/../lib", "-Wl,-install_name,@rpath/lib" + name + ".so"]

    rpath_flags = select({
        "//bazel/config:linux_aarch64": linux_rpath_flags,
        "//bazel/config:linux_x86_64": linux_rpath_flags,
        "//bazel/config:linux_ppc64le": linux_rpath_flags,
        "//bazel/config:linux_s390x": linux_rpath_flags,
        "//bazel/config:windows_x86_64": [],
        "//bazel/config:macos_x86_64": macos_rpath_flags,
        "//bazel/config:macos_aarch64": macos_rpath_flags,
    })

    # Create a cc_library entry to generate a shared archive of the target (linux).
    native.cc_library(
        name = name + ".so",
        srcs = srcs,
        hdrs = hdrs + fincludes_hdr,
        deps = deps,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + copts + fincludes_copt,
        data = data,
        tags = tags,
        linkopts = MONGO_GLOBAL_LINKFLAGS + linkopts,
        linkstatic = True,
        local_defines = MONGO_GLOBAL_DEFINES + visibility_support_defines + local_defines,
        includes = includes,
        features = ["supports_pic", "pic"],
        target_compatible_with = select({
            "//bazel/config:shared_archive_enabled_linux": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
    )

    # Create a cc_library entry to generate a shared archive of the target (windows).
    native.cc_library(
        name = name + ".dll",
        srcs = srcs,
        hdrs = hdrs + fincludes_hdr,
        deps = deps,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + copts + fincludes_copt,
        data = data,
        tags = tags,
        linkopts = MONGO_GLOBAL_LINKFLAGS + linkopts,
        linkstatic = True,
        local_defines = MONGO_GLOBAL_DEFINES + visibility_support_defines + local_defines,
        includes = includes,
        features = ["supports_pic", "pic"],
        target_compatible_with = select({
            "//bazel/config:shared_archive_enabled_windows": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
    )

    deps_with_shared_archive = deps + select({
        "//bazel/config:shared_archive_enabled_linux": [":" + name + ".so"],
        "//bazel/config:shared_archive_enabled_windows": [":" + name + ".dll"],
        "//conditions:default": [],
    })

    native.cc_library(
        name = name + WITH_DEBUG_SUFFIX,
        srcs = srcs,
        hdrs = hdrs + fincludes_hdr,
        deps = deps_with_shared_archive,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + copts + fincludes_copt,
        data = data,
        tags = tags,
        linkopts = MONGO_GLOBAL_LINKFLAGS + linkopts,
        linkstatic = True,
        local_defines = MONGO_GLOBAL_DEFINES + local_defines,
        includes = includes,
        features = select({
            "//bazel/config:linkstatic_disabled": ["supports_pic", "pic"],
            "//bazel/config:shared_archive_enabled": ["supports_pic", "pic"],
            "//conditions:default": ["pie"],
        }),
    )

    # Creates a shared library version of our target only if //bazel/config:linkstatic_disabled is true.
    # This uses the CcSharedLibraryInfo provided from extract_debuginfo to allow it to declare all dependencies in
    # dynamic_deps.
    native.cc_shared_library(
        name = name + CC_SHARED_LIBRARY_SUFFIX + WITH_DEBUG_SUFFIX,
        deps = [name + WITH_DEBUG_SUFFIX],
        visibility = visibility,
        tags = tags,
        user_link_flags = MONGO_GLOBAL_LINKFLAGS + linkopts + rpath_flags + visibility_support_shared_flags,
        target_compatible_with = select({
            "//bazel/config:linkstatic_disabled": [],
            "//conditions:default": ["@platforms//:incompatible"],
        }),
        dynamic_deps = deps,
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
        local_defines = []):
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
    """

    if linkstatic == True:
        fail("""Linking specific targets statically is not supported.
        The mongo build must link entirely statically or entirely dynamically.
        This can be configured via //config/bazel:linkstatic.""")

    fincludes_copt = force_includes_copt(native.package_name(), name)
    fincludes_hdr = force_includes_hdr(native.package_name(), name)

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
        srcs = srcs + fincludes_hdr,
        deps = all_deps,
        visibility = visibility,
        testonly = testonly,
        copts = MONGO_GLOBAL_COPTS + copts + fincludes_copt,
        data = data,
        tags = tags,
        linkopts = MONGO_GLOBAL_LINKFLAGS + linkopts + rpath_flags,
        linkstatic = LINKSTATIC_ENABLED,
        local_defines = MONGO_GLOBAL_DEFINES + LIBUNWIND_DEFINES + local_defines,
        includes = includes,
        features = ["pie"],
        dynamic_deps = select({
            "//bazel/config:linkstatic_disabled": deps,
            "//conditions:default": [],
        }),
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
